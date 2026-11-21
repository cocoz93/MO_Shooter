#pragma once

#include "AnimClips.h"
#include "AnimPose.h"
#include "GameProtocol.h"

#include <cstdint>
#include <vector>

// 입력 → 애니 상태. 서버가 틱마다 돌리고 결과를 되감기 기록과 패킷에 싣는다(D32).
// - 옮긴 원본 = MOBSimpleAnimInstance.cpp 의 UpdateIdleBreak ~ UpdateJump 와 PreUpdate 의 입력 처리.
//   같은 입력에 언리얼과 같은 상태가 나오는지는 Tools/AnimCheck state + check_pose.py 로 대조한다.
// - 몽타주(9단계 3-b) = 엔진 FAlphaBlend · FAnimMontageInstance(Play · Stop · UpdateWeight · Advance) · Montage_PlayInternal 의 무리 끄기를 옮겼다.
//   정답지는 Lyra 기록 animcmp_ev_<무기>_probe.csv 의 몽타주 칸이다(AnimCheck state --probe).
// - 조준(컨텐츠 C-3, D60) = 단순 재생(MOBSimpleAnimInstance)에는 없는 몫이라 Lyra 기록 animcmp_ads_<무기>_probe.csv 와 걸음 상태 기계 전환 표에서 바로 옮겼다.
// - 앉기(컨텐츠 D-3, D62) = 같은 식으로 Lyra 기록 animcmp_crouch_<무기>_probe.csv 와 전환 표에서 옮겼다. 앉기 묶음이 조준 묶음보다 먼저다.
// - 언리얼 안에서도 컴파일돼야 하므로 예외·RTTI 를 쓰지 않는다. 사람 하나에 객체 하나를 쓴다.
namespace mo
{
	inline constexpr int kAnimJumpClips = 5;			// 뛰어오름 · 오르는 중 · 꼭대기 · 떨어지는 중 · 내려앉음
	inline constexpr int kAnimTransitions = 12;			// 네 방향 × 시작 · 멈춤 · 방향 꺾기
	inline constexpr int kAnimTransitionClips = kAnimTransitions * 3;			// 허리 12 + 조준 12(D60) + 앉음 12(D62). 전환 번호 = 쌓기 칸 번호 − kAnimJumpClips
	inline constexpr int kAnimStanceEntry = kAnimJumpClips + kAnimTransitionClips;	// 41 — 서기 안 앉기 전환의 쌓기 칸 번호(D62 4)
	inline constexpr int kAnimStanceExit = kAnimStanceEntry + 1;				// 42 — 서기 안 일어서기 전환
	inline constexpr int kAnimStackClips = kAnimStanceExit + 1;				// 43
	inline constexpr int kAnimBaseClips = kAnimWalkClips * 3;					// 서기 · 걸음 넷 × 허리 · 조준 · 앉음. 조준 칸 = 허리 칸 + kAnimWalkClips, 앉음 칸 = 허리 칸 + 2 × kAnimWalkClips
	static_assert(kAnimStackClips == kStackClipCount && kAnimJumpClips + kAnimTransitions == kStackHipClips && kAnimJumpClips + kAnimTransitions * 2 == kStackAdsEnd,
		"쌓기 칸 번호가 AnimClips.h StackClipIndex 와 같아야 한다");

	// 한 프레임의 입력. 속도와 가속은 몸 기준(요 0) 값이다
	struct AnimMoveInput
	{
		float deltaSeconds = 0.f;
		double velocity[3] = {};
		double acceleration[3] = {};
		bool accelerating = false;			// 이동 입력이 있나
		bool falling = false;
		float pitch = 0.f;					// 시선 위아래 각(-90~90)
		float groundDistance = 0.f;			// 떨어지는 중일 때 땅까지 거리(cm)
		bool hasFloorGap = false;			// 땅에서 바닥을 찾았나
		float plantGap = 0.f;				// 메시 뿌리가 바닥보다 얼마나 위인가(cm)
		float airGap = 0.f;					// 떠 있을 때 메시 뿌리가 캡슐 바닥보다 얼마나 위인가(메시 자리 z + 캡슐 반높이, cm) — 뜬 동안 땅 = −(airGap + groundDistance)(L-6-c-3)
		bool ads = false;					// 조준 중인가 — 이 갱신 앞에 정해진 조준 능력 상태(서버 = 이 틱 입력을 적용한 뒤 값, D61). Lyra 는 능력이 바뀐 뒤 첫 갱신이 본다(실측 — 기록 도구가 프레임 끝에 켜서 기록에선 한 줄 앞 값)
		bool crouched = false;				// 앉았나 — 이 갱신 앞 이동이 끝낸 앉음(언리얼 bIsCrouched, D62 8). Lyra 기록은 앉음이 바뀐 프레임에 애니도 바뀐다(animcmp_crouch_rifle_probe.csv c_on · c_off, 실측)
	};

	class CAnimState
	{
	public:
		// 클립 자료와 묶음을 고른다. 클립 수가 다르거나 발 표시 · 거리 곡선이 없거나, 무기별 시각 표에 없는 묶음이면 false
		bool Init(const CAnimClips& clips, const char* bundleName);
		// 무기를 바꾼 틱 — 묶음을 바꾸고 재생 상태를 Lyra 가 무기 층을 바꿀 때처럼 옮긴다(Update 앞에 부른다). false 면 상태는 그대로다
		bool ChangeBundle(const CAnimClips& clips, const char* bundleName);
		void Reset();
		// evaluator = 이 상태의 묶음(BundleIndex)으로 Init 한 자세 계산기, scratch = 그 계산기 뼈 수만큼의 작업 칸.
		// 발 붙이기 장치(L-6-c-2)가 이번 갱신 끝 자세(발 디딤 앞)의 발 높이를 읽어야 해서 갱신마다 자세를 한 번 셈한다 — Lyra 장치도 그 프레임 자세를 읽는다
		void Update(const AnimMoveInput& input, CAnimPoseEvaluator& evaluator, BoneTransform* scratch);
		void ToPoseInput(AnimPoseInput& out) const;

		// 패킷 · 되감기 기록에 싣는 형식. 서버 판정과 화면이 둘 다 이 값으로 자세를 만들어야 양쪽이 같은 수를 본다(D32 · D35)
		void ToWire(wire::AnimStateWire& out) const;
		// 받은 상태 → 자세 입력. 이 객체의 표(클립 길이 · 발 표시)만 읽고 재생 상태는 안 건드린다. pitch · crouched 는 PlayerState 에 이미 있는 값이다(D62 8 — 앉음은 쏜 뒤 덮기 자세를 고른다)
		void FromWire(const wire::AnimStateWire& in, float pitch, bool crouched, AnimPoseInput& out) const;

		int StackOverflows() const { return _stackOverflows; }

		// 몽타주를 튼다 — 엔진 UAnimInstance::Montage_PlayInternal 규칙: 같은 무리의 몽타주를 새 몽타주의 넣기 섞기(시간 · 곡선)로 끄고 새 칸을 뒤에 붙인다.
		// Update 뒤에 부른다. Lyra 는 그 프레임의 갱신 · 자세 계산이 끝난 뒤 사건에서 틀어, 새 칸은 시각 0 · 비중 0 으로 생기고 다음 갱신부터 움직인다(3-b 기록, 실측).
		// playRate = 트는 쪽이 준 속도(무기 표의 값). 실제 속도는 이 값 × 자산 배율(ClipMontage::rate)이다.
		// bundle = 몽타주를 찾을 묶음 번호(CAnimClips::Bundles() 차례), -1 = 지금 묶음. 그 묶음에 그 종류가 없으면 false
		bool PlayMontage(uint8_t kind, float playRate, int bundle = -1);
		// 도는 몽타주 하나를 자산의 빼기 시간 · 곡선으로 끈다 — 능력이 취소될 때의 길(엔진 Montage_Stop + 능력 쪽 CurrentMontageStop 이 자산 빼기 시간을 넘긴다).
		// 엔진처럼 그 몽타주의 안 꺼진 칸 중 가장 최근 것 하나만 끈다(ActiveMontagesMap). Update 뒤, 같은 사건의 PlayMontage 앞에 부른다.
		// 재장전 중 쏘면 Lyra 는 재장전을 이 길로 먼저 끄고 쏘기를 튼다 — 3-b 1053 기록의 재장전 칸 「비중 1 · 빠지는 중 · 섞기 시간 0」은 이 두 단계로만 나온다(실측).
		// 끌 칸이 없으면 false
		bool StopMontageKind(uint8_t kind, int bundle = -1);
		// 쏘기 사건 — 들어오는 중 · 다 들어온 재장전 칸이 있으면 그 칸 하나를 StopMontageKind 로 먼저 끄고 쏘기를 튼다. 서버와 대조 도구(AnimCheck)가 같이 쓴다.
		// cancelledReload = 재장전을 끊었나. 반환 = PlayMontage 결과
		bool PlayFireMontage(float playRate, int bundle, bool& cancelledReload);
		// 재장전 능력을 끊는 능력의 몽타주 — 쏘기와 같은 길로 재장전 칸을 먼저 끄고 kind 를 튼다. 근접(E-3)이 쓴다(근접도 재장전을 끊는다, TagRelationships_ShooterHero)
		// 수류탄(G-4)도 쓴다 — 켤 때 재장전을 끊는다(G-0-c). 던짐 넣기 곡선(1)과 재장전 빼기 곡선(2)이 달라 이 길이라야 재장전 칸이 Lyra 와 같은 프레임에 빠진다(G-4-b AnimCheck)
		bool PlayMontageCancellingReload(uint8_t kind, float playRate, int bundle, bool& cancelledReload);
		// 발사 능력이 켜져 있나(Lyra 태그 IsFiring). Update 뒤에 부른다 — 다음 Update 의 「쏜 뒤 시간」이 이 값을 본다
		void SetFiring(bool firing) { _firing = firing; }

		// 대조 시험 · 기록용으로 읽는 값. 몽타주 칸은 사건(PlayMontage)까지 반영한 지금 목록이다 — 자세는 사건 앞 목록(ToPoseInput)으로 만든다
		struct MontageView
		{
			const ClipMontage* montage = nullptr;
			int bundle = -1;
			float position = 0.f;
			float weight = 0.f;
			float desired = 0.f;		// 1 = 들어오는 중 · 다 들어옴, 0 = 빠지는 중
			float blendTime = 0.f;
			float playRate = 1.f;
		};
		int MontageCount() const { return _montageCount; }
		MontageView MontageAt(int index) const;
		int MontageDrops() const { return _montageDrops; }		// 칸이 차서 버린 몽타주 수(누적). 0 이어야 Lyra 와 같다
		int ProfileStartMisses() const { return _profileStartMisses; }	// 빼기 비중 표가 걸린 빼기 중 시작 진행이 표의 배율 아래라 자세 계산(시작 1 로 봄)이 엔진과 다른 수(누적, L-6-b). Lyra 기록은 0
		int BundleIndex() const { return _bundleIndex; }
		float TimeSinceFired() const { return _timeSinceFired; }
		float OverrideWeight() const { return _overrideWeight; }
		float AimBlend() const { return _aimBlend; }
		float AdsBlend() const { return _adsBlend; }		// 조준 묶음 비중(D60) — 0 = 허리 클립만, 1 = 조준 클립만
		float CrouchBlend() const { return _crouchBlend; }	// 앉기 묶음 비중(D62 2) — 조준 묶음보다 먼저 나눈다
		// 이동 상태 이름 — Lyra LocomotionSM 의 상태 이름과 같은 낱말(공중은 Lyra 점프 상태 다섯을 묶은 "Air"). 대조 도구가 Lyra 기록과 비교한다
		const char* MoveStateName() const;
		bool MoveAds() const { return _moveAds; }		// 지금 전환 클립을 고른 조준 여부(D60)
		bool MoveCrouch() const { return _moveCrouch; }	// 지금 전환 클립을 고른 앉음 여부(D62 3)
		// 쌓기에 서기 안 앉기 · 일어서기 전환 칸이 있나(D62 4) — Lyra IdleStance 의 StanceTransition 과 대조한다. 0 = 없음, 1 = 앉기, 2 = 일어서기(맨 위 것).
		// 서기로 돌아가며 빠지는 중인 칸(settled)은 안 센다 — Lyra 는 그 섞기가 시작되는 프레임에 상태 이름을 Idle 로 바꾼다
		int StanceLayer() const;
		float HandLeftAlpha() const { return _handLeftAlpha; }
		float LegIKAlpha() const { return _legIKAlpha; }		// 다리 IK 비중 = 1 − 앞 자세 곡선 DisableLegIK(L-12-c)
		// 발 붙이기 장치의 이번 갱신 값(대조 도구 기록용). stepped = 이번 갱신에 스프링을 넘겼나(뜬 프레임 · 알파 0 이면 멈춤)
		struct PlantView
		{
			double feet[2] = {};		// 발 높이(FootHeightsFromLocal) — 넘기지 않은 갱신은 앞 값
			float ground = 0.f;			// 부품 공간 땅 높이 = -plantGap
			float target = 0.f;			// 골반 목표 = min(왼 목표, 오른 목표)
			float current = 0.f;		// 스프링 값(알파 곱하기 전)
			float alpha = 1.f;
			bool stepped = false;
		};
		PlantView Plant() const;
		uint64_t PlantBundleMisses() const { return _plantBundleMisses; }		// 다른 묶음 계산기를 받아 발 높이를 못 읽은 갱신 수(누적). 0 이어야 한다
		float DisableLeftHand() const { return _disableLeftHand; }		// 이번 자세의 DisableLHandIK 곡선(사건 앞 목록으로)

		// 대조 시험의 일부러 틀림용 — 제동 감속을 배율만큼 바꾼다
		void SetBrakingScaleForTest(float scale) { _brakingDecelerationWalking *= scale; }
		// 대조 시험의 일부러 틀림용(3-b) — kMontageTest* 를 더한 값
		static constexpr int kMontageTestLateFrame = 1;		// 새 몽타주를 한 프레임(1/60초) 뒤 시각에서 시작
		static constexpr int kMontageTestNoGroup = 2;		// 같은 무리를 안 끈다
		static constexpr int kMontageTestLinear = 4;		// 넣기 · 빼기 곡선을 다 선형으로
		void SetMontageTestFlags(int flags) { _montageTest = flags; }
		// 서기 안 전환을 넣고 빼는 섞기 시간 후보 고르기용(D62 4 — 22부위로 고른다). 실제 경로 = 넣기 0 · 빼기 0.25초(D63).
		// in 0 = 바로 얹는다. out > 0 이면 그 칸이 맨 위일 때 서기 자리를 그 시간에 걸쳐 덮고, 전환 클립은 끝까지 돈다. out 0 = 그 자리를 서기로 바로 바꾼다
		void SetStanceFadeForTest(float inSeconds, float outSeconds) { _stanceInSeconds = inSeconds; _stanceOutSeconds = outSeconds; }

	private:
		// 엔진 FAlphaBlend 에서 몽타주 비중이 쓰는 몫(사용자 곡선 · 관성 섞기는 안 옮긴다 — D48)
		struct MontageBlend
		{
			float begin = 0.f;
			float desired = 1.f;
			float blended = 0.f;
			float alpha = 0.f;
			float blendTime = 0.2f;			// 엔진 기본값
			float remaining = 0.2f;
			uint8_t option = 2;				// 엔진 기본값 HermiteCubic
			bool resetAlpha = false;
			bool resetTime = false;

			void SetValueRange(float beginValue, float desiredValue);
			void SetBlendTime(float seconds);
			void SetAlpha(float value);
			void Update(float deltaSeconds);
			bool IsComplete() const;
		};

		// 엔진 FAnimMontageInstance 에서 구역 하나 · 분기점 없음 · 뿌리 이동 없음인 몽타주가 쓰는 몫
		struct MontageInstance
		{
			const ClipMontage* montage = nullptr;
			int bundle = -1;
			float position = 0.f;
			float playRate = 1.f;
			float timeLeft = 0.f;			// 엔진 FMontageSubStepper::TimeRemaining — 이번 갱신에 아직 못 쓴 시간
			bool playing = false;
			bool autoBlendOut = true;
			int16_t profile = -1;			// 걸린 빼기 비중 표(엔진 ActiveBlendProfile) — 제 빼기로 빠질 때만 몽타주의 blendOutProfile, 섞기가 끝나면 -1(L-6-b)
			MontageBlend blend;
		};

		enum class EMoveState : uint8_t { Idle, Start, Cycle, Stop, Pivot, Air };

		struct Layer
		{
			int clip = -1;
			float time = 0.f;
			float fade = 1.f;
			float fadeSeconds = 0.f;
			bool settled = false;		// 서기 안 전환 칸이 이미 서기 자리로 넘겼다(빼기 섞기 후보에서만 — 다시 안 넘기게)
		};

		bool LoadBundle(const CAnimClips& clips, const char* bundleName);		// 표만 읽는다 — 재생 상태는 안 건드린다
		void StepSpan(int clip, int step, float& outStart, float& outEnd) const;
		bool FollowStepsOfTransition(float deltaSeconds);
		void UpdateMotion(const AnimMoveInput& input, float deltaSeconds);		// 전환 클립 시각 공식이 쓰는 이번 프레임 이동 값(_frameTravel 등)
		void UpdateTransitions(const AnimMoveInput& input, float deltaSeconds);
		void UpdateJump(bool falling, bool accelerating, float velocityZ, float timeToApex, float groundDistance, float deltaSeconds);
		void UpdateIdleBreak(bool still, bool noIdleBreak, float deltaSeconds);
		void PushJump(int clip, float fadeSeconds);
		void PushStance(bool crouched);
		void SettleStance();
		void EnterTransition(EMoveState state, int direction, float fadeSeconds);
		void EnterLocomotion(EMoveState state, float fadeSeconds);
		void JumpWeights(float out[kAnimLayerMax]) const;
		float TransitionClipTime(const Layer& layer, float deltaSeconds) const;
		float TimeFromDistance(int transition, float value) const;
		float DistanceAtTime(int transition, float time) const;
		int StepMarkerAt(int transition, float time) const;
		float LandTimeFromDistance(float groundDistance) const;
		bool HasJumpLayer() const;
		float PredictStopDistance(float speed) const;
		float PredictPivotDistance(const double acceleration[3], const double velocity[3]) const;
		void UpdateFiring(bool accelerating, bool falling, bool ads, bool crouched, float deltaSeconds);
		void UpdateMontages(float deltaSeconds);
		void StepPlant(float ground, float deltaSeconds, CAnimPoseEvaluator& evaluator, BoneTransform* scratch);
		bool AdvanceMontage(MontageInstance& instance, float deltaSeconds);		// false = 다 빠졌다(엔진 Terminate)
		void StopMontage(MontageInstance& instance, uint8_t option, float blendSeconds, int16_t profile);
		float ProfileMaxFactor(int profile) const;
		void DropMontage();
		void SnapshotMontages();
		bool FullBodyCoversSource(float deltaSeconds);		// 이번 갱신에 통째(FullBody) 자리 몽타주가 몸을 다 덮나 — 그러면 걸음 쪽 그래프를 안 넘긴다
		void ReinitLocomotion();							// 덮였던 걸음 쪽이 다시 갱신되는 첫 프레임 — 엔진 상태 기계 다시 초기화
		float PoseCurve(const char* name) const;			// 이번 자세 목록의 몽타주 곡선 × 비중의 합 — DisableLHandIK(왼손) · DisableLegIK(발 붙이기 장치, L-6-c-1)
		float StackLegCurve() const;						// 쌓기 칸(점프 클립) DisableLegIK × 칸 비중 × 통째 자리 몽타주가 안 덮은 몫(L-6-c-3)

		// 클립에서 읽어 둔 값
		const CAnimClips* _clips = nullptr;
		int _bundleIndex = -1;
		int _fullBodySlot = -1;			// 끼움 자리 표의 FullBody 번호(-1 = 없음)
		float _lengths[kAnimBaseClips] = {};				// 허리 서기 · 걸음 넷, 조준 서기 · 걸음 넷, 앉은 서기 · 걸음 넷
		float _rootSpeeds[kAnimBaseClips] = {};
		std::vector<float> _stepTimes[kAnimBaseClips];		// 왼발 표시부터 차례로. 클립 끝을 넘어가면 길이를 더해 계속 커진다
		int _stepCycle = 1;									// 걸음 클립 열둘(허리 · 조준 · 앉음)의 발 표시 수의 최소공배수
		float _stackLengths[kAnimStackClips] = {};
		const std::vector<float>* _transitionDistances[kAnimTransitionClips] = {};
		const std::vector<ClipMarker>* _transitionMarkers[kAnimTransitionClips] = {};
		const std::vector<float>* _landDistances = nullptr;
		float _recoveryLength = 0.f;
		float _fidgetLength = 0.f;
		float _groundFriction = 0.f;
		float _brakingFriction = 0.f;
		float _brakingFrictionFactor = 0.f;
		float _brakingDecelerationWalking = 0.f;
		float _gravity = 0.f;
		bool _separateBraking = false;
		// 묶음 이름으로 고른 무기별 시각(AnimState.cpp kBundleTimings)
		float _stopClipSeconds = 0.f;
		float _exitTimes[3][3][4] = {};		// [허리 · 조준 · 앉음][출발 · 멈춤 · 돌아서기][앞 · 뒤 · 왼 · 오른]
		bool _raiseWhenCrouched = false;	// Lyra 무기 층 RaiseWeaponAfterFiringWhenCrouched(D62 5)

		// 상태
		float _weights[kAnimWalkClips] = { 1.f, 0.f, 0.f, 0.f, 0.f };
		float _times[kAnimWalkClips] = {};
		float _adsTimes[kAnimWalkClips] = {};		// 조준 서기 · 걸음 넷의 시각 — 걸음 번호 · 진행률 · 서기 시계를 같이 쓴다(D60 2)
		float _adsBlend = 0.f;						// 조준 묶음 비중 — 방향 비중과 같은 따라가기
		bool _ads = false;							// 이번 갱신이 본 조준
		bool _moveAds = false;						// 지금 전환 클립이 조준 묶음인가 — 들어갈 때 고른다(D60 3)
		float _crouchTimes[kAnimWalkClips] = {};	// 앉은 서기 · 걸음 넷의 시각 — 조준과 같은 걸음 번호 · 진행률 · 서기 시계(D62 2)
		float _crouchBlend = 0.f;					// 앉기 묶음 비중 — 방향 비중과 같은 따라가기
		bool _crouch = false;						// 이번 갱신이 본 앉음
		bool _moveCrouch = false;					// 지금 전환 클립이 앉음 묶음인가 — 들어갈 때 고른다(D62 3)
		float _stanceInSeconds = 0.f;				// 서기 안 전환을 넣는 섞기 — Lyra 는 바꾼 프레임에 비중 1.00(D-3-a 기록, 실측)
		float _stanceOutSeconds = 0.25f;			// 빼는 섞기 = 엔진 자동 전환의 섞기 시간(남은 시간 문턱과 같은 값, D63)
		int _step = 0;
		float _stepFrac = 0.f;
		float _idleTime = 0.f;
		// 이번 갱신의 시각을 만든 값. 이 뒤에 전환이 _step · _idleTime 을 바꿔도 그 효과는 다음 갱신부터이므로 패킷엔 이 값을 싣는다
		int _timesStep = 0;
		float _timesStepFrac = 0.f;
		float _timesIdle = 0.f;
		float _aimBlend = 1.f;
		float _pitch = 0.f;
		Layer _stack[kAnimLayerMax];
		int _count = 0;
		bool _wasFalling = false;
		float _recoveryTime = -1.f;
		float _standTime = 0.f;
		float _fidgetTime = -1.f;
		bool _fidgetLeaving = false;
		EMoveState _moveState = EMoveState::Idle;
		int _moveDirection = 0;
		double _pivotAccelDirection[2] = {};
		bool _beforePivot = false;
		int _followClip = -1;
		int _followBaseStep = 0;
		int _followBaseMarker = 0;
		float _frameTravel = 0.f;
		float _stopDistance = 0.f;
		float _pivotDistance = 0.f;
		bool _accelerating = false;		// 이번 프레임 가속 입력 — 빠지는 멈춤 클립의 거리 맞추기 조건(ShouldDistanceMatchStop)
		float _plantDrop = 0.f;
		float _plantGround = 0.f;			// 이번 갱신 장치가 쓴 땅(부품 공간 cm) — 땅 = −발 디딤 거리 · 뜬 동안 = −(airGap + 땅까지 거리)
		float _plantOffset[3] = {};
		float _plantAlpha = 1.f;			// 발 붙이기 장치 켜짐(엔진 컨트롤 릭 노드 알파, 0.2초 선형) — 앞 자세 곡선 DisableLegIK 가 0 보다 크면 0 쪽으로(L-6-c-1)
		float _disableLegIK = 0.f;			// 이번 자세의 DisableLegIK 곡선 — 다음 갱신의 장치 켜짐을 정한다
		// 장치 골반 스프링(L-6-c-2) — 엔진 FloatSpringInterp 의 상태(FFloatSpringState)와 컨트롤 릭 변수 CurrentPelvisOffsetZ
		float _plantCurrent = 0.f;			// 부품 공간 골반 이동(cm, 음수 = 내림) — 알파를 곱하기 전
		float _plantVelocity = 0.f;
		float _plantPrevTarget = 0.f;
		bool _plantPrevValid = false;		// false = 판 처음 — 첫 걸음은 목표 속도 0(엔진 FFloatSpringState::bPrevTargetValid)
		double _plantFeet[2] = {};
		float _plantTarget = 0.f;
		bool _plantStepped = false;
		uint64_t _plantBundleMisses = 0;
		int _stackOverflows = 0;
		bool _sourceFrozen = false;		// 앞 갱신에 통째 자리 몽타주가 몸을 다 덮어 걸음 쪽을 안 넘겼다
		bool _reinitIdle = false;		// 걸음 쪽을 다시 초기화한 뒤 아직 서기 상태 — 걸음 목표를 서기 클립만으로(L-6-b-2)

		// 몽타주 · 쏜 뒤 덮기(9단계 3-b)
		MontageInstance _montages[kAnimMontageMax];
		int _montageCount = 0;
		AnimMontageSlot _poseMontages[kAnimMontageMax];		// 이번 갱신이 끝난 때(사건 앞)의 목록 — 자세 · 곡선은 이것으로
		int _poseMontageCount = 0;
		float _disableLeftHand = 0.f;		// 이번 자세의 DisableLHandIK 곡선 — 다음 갱신의 왼손 비중이 된다
		float _handLeftAlpha = 1.f;
		float _legIKAlpha = 1.f;			// 다리 IK 비중(L-12-c) — 앞 자세의 _disableLegIK 에서 왼손 비중과 같은 때 낸다
		bool _handFresh = false;			// 묶음을 바꾼 뒤 첫 갱신 — 새 층은 아직 곡선을 읽은 적이 없어 왼손 비중이 1 이다
		float _timeSinceFired = 0.f;
		float _overrideWeight = 0.f;
		bool _firing = false;
		int _montageDrops = 0;
		int _profileStartMisses = 0;
		int _montageTest = 0;
	};

	// 그 자세에 맞은 쪽 반응 몽타주(MONTAGE_HIT_BACK · MONTAGE_HIT_FRONT) 칸이 실렸나 — 서버 REWIND react= 와 봇 BOTSHOT targetReact= 가
	// 이 한 정의로 「상대가 반응 중」인 쏨을 따로 센다(E-3 17-d). 반응은 근접에 맞을 때만 켜진다(Room.cpp EMeleeHit::Hit)
	inline bool HasHitReactMontage(const wire::AnimStateWire& anim)
	{
		constexpr uint8_t kKindMask = (uint8_t)((1 << wire::kAnimMontageKindBits) - 1);
		for (uint8_t i = 0; i < anim.montageCount && i < wire::kAnimMontagesOnWire; ++i)
		{
			const uint8_t kind = (uint8_t)(anim.montages[i].montage & kKindMask);
			if (kind == MONTAGE_HIT_BACK || kind == MONTAGE_HIT_FRONT) return true;
		}
		return false;
	}

	// 그 자세에 대시 몽타주(MONTAGE_DASH_FORWARD ~ RIGHT) 칸이 실렸나 — 봇 BOTSHOT targetDash= 가 「상대가 대시 중」인 쏨을 따로 센다(컨텐츠 D-6).
	// 몽타주는 힘(0.25초)보다 오래 산다(빼기 포함 53~59프레임, D104) — 빠르게 미끄러지는 몫과 그 뒤 빼기 몫을 같이 센다
	inline bool HasDashMontage(const wire::AnimStateWire& anim)
	{
		constexpr uint8_t kKindMask = (uint8_t)((1 << wire::kAnimMontageKindBits) - 1);
		for (uint8_t i = 0; i < anim.montageCount && i < wire::kAnimMontagesOnWire; ++i)
		{
			const uint8_t kind = (uint8_t)(anim.montages[i].montage & kKindMask);
			if (kind >= MONTAGE_DASH_FORWARD && kind <= MONTAGE_DASH_RIGHT) return true;
		}
		return false;
	}
}
