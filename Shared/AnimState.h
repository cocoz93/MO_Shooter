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
// - 언리얼 안에서도 컴파일돼야 하므로 예외·RTTI 를 쓰지 않는다. 사람 하나에 객체 하나를 쓴다.
namespace mo
{
	inline constexpr int kAnimJumpClips = 5;			// 뛰어오름 · 오르는 중 · 꼭대기 · 떨어지는 중 · 내려앉음
	inline constexpr int kAnimTransitions = 12;			// 네 방향 × 시작 · 멈춤 · 방향 꺾기
	inline constexpr int kAnimStackClips = kAnimJumpClips + kAnimTransitions;

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
	};

	class CAnimState
	{
	public:
		// 클립 자료와 묶음을 고른다. 클립 수가 다르거나 발 표시 · 거리 곡선이 없거나, 무기별 시각 표에 없는 묶음이면 false
		bool Init(const CAnimClips& clips, const char* bundleName);
		// 무기를 바꾼 틱 — 묶음을 바꾸고 재생 상태를 Lyra 가 무기 층을 바꿀 때처럼 옮긴다(Update 앞에 부른다). false 면 상태는 그대로다
		bool ChangeBundle(const CAnimClips& clips, const char* bundleName);
		void Reset();
		void Update(const AnimMoveInput& input);
		void ToPoseInput(AnimPoseInput& out) const;

		// 패킷 · 되감기 기록에 싣는 형식. 서버 판정과 화면이 둘 다 이 값으로 자세를 만들어야 양쪽이 같은 수를 본다(D32 · D35)
		void ToWire(wire::AnimStateWire& out) const;
		// 받은 상태 → 자세 입력. 이 객체의 표(클립 길이 · 발 표시)만 읽고 재생 상태는 안 건드린다. pitch 는 PlayerState 에 이미 있는 값이다
		void FromWire(const wire::AnimStateWire& in, float pitch, AnimPoseInput& out) const;

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
		int BundleIndex() const { return _bundleIndex; }
		float TimeSinceFired() const { return _timeSinceFired; }
		float OverrideWeight() const { return _overrideWeight; }
		float AimBlend() const { return _aimBlend; }
		float HandLeftAlpha() const { return _handLeftAlpha; }
		float DisableLeftHand() const { return _disableLeftHand; }		// 이번 자세의 DisableLHandIK 곡선(사건 앞 목록으로)

		// 대조 시험의 일부러 틀림용 — 제동 감속을 배율만큼 바꾼다
		void SetBrakingScaleForTest(float scale) { _brakingDecelerationWalking *= scale; }
		// 대조 시험의 일부러 틀림용(3-b) — kMontageTest* 를 더한 값
		static constexpr int kMontageTestLateFrame = 1;		// 새 몽타주를 한 프레임(1/60초) 뒤 시각에서 시작
		static constexpr int kMontageTestNoGroup = 2;		// 같은 무리를 안 끈다
		static constexpr int kMontageTestLinear = 4;		// 넣기 · 빼기 곡선을 다 선형으로
		void SetMontageTestFlags(int flags) { _montageTest = flags; }

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
			MontageBlend blend;
		};

		enum class EMoveState : uint8_t { Idle, Start, Cycle, Stop, Pivot, Air };

		struct Layer
		{
			int clip = -1;
			float time = 0.f;
			float fade = 1.f;
			float fadeSeconds = 0.f;
		};

		bool LoadBundle(const CAnimClips& clips, const char* bundleName);		// 표만 읽는다 — 재생 상태는 안 건드린다
		void StepSpan(int clip, int step, float& outStart, float& outEnd) const;
		bool FollowStepsOfTransition(float deltaSeconds);
		void UpdateTransitions(const AnimMoveInput& input, float deltaSeconds);
		void UpdateJump(bool falling, bool accelerating, float velocityZ, float timeToApex, float groundDistance, float deltaSeconds);
		void UpdateIdleBreak(bool still, float deltaSeconds);
		void PushJump(int clip, float fadeSeconds);
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
		void UpdateFiring(bool accelerating, float deltaSeconds);
		void UpdateMontages(float deltaSeconds);
		bool AdvanceMontage(MontageInstance& instance, float deltaSeconds);		// false = 다 빠졌다(엔진 Terminate)
		void StopMontage(MontageInstance& instance, uint8_t option, float blendSeconds);
		void DropMontage();
		void SnapshotMontages();
		float LeftHandCurve() const;

		// 클립에서 읽어 둔 값
		const CAnimClips* _clips = nullptr;
		int _bundleIndex = -1;
		float _lengths[kAnimWalkClips] = {};
		float _rootSpeeds[kAnimWalkClips] = {};
		std::vector<float> _stepTimes[kAnimWalkClips];		// 왼발 표시부터 차례로. 클립 끝을 넘어가면 길이를 더해 계속 커진다
		int _stepCycle = 1;
		float _stackLengths[kAnimStackClips] = {};
		const std::vector<float>* _transitionDistances[kAnimTransitions] = {};
		const std::vector<ClipMarker>* _transitionMarkers[kAnimTransitions] = {};
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
		float _exitTimes[3][4] = {};		// [출발 · 멈춤 · 돌아서기][앞 · 뒤 · 왼 · 오른]

		// 상태
		float _weights[kAnimWalkClips] = { 1.f, 0.f, 0.f, 0.f, 0.f };
		float _times[kAnimWalkClips] = {};
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
		float _plantDrop = 0.f;
		float _groundTime = 0.f;
		float _airTime = 0.f;
		float _plantOffset[3] = {};
		int _stackOverflows = 0;

		// 몽타주 · 쏜 뒤 덮기(9단계 3-b)
		MontageInstance _montages[kAnimMontageMax];
		int _montageCount = 0;
		AnimMontageSlot _poseMontages[kAnimMontageMax];		// 이번 갱신이 끝난 때(사건 앞)의 목록 — 자세 · 곡선은 이것으로
		int _poseMontageCount = 0;
		float _disableLeftHand = 0.f;		// 이번 자세의 DisableLHandIK 곡선 — 다음 갱신의 왼손 비중이 된다
		float _handLeftAlpha = 1.f;
		bool _handFresh = false;			// 묶음을 바꾼 뒤 첫 갱신 — 새 층은 아직 곡선을 읽은 적이 없어 왼손 비중이 1 이다
		float _timeSinceFired = 0.f;
		float _overrideWeight = 0.f;
		bool _firing = false;
		int _montageDrops = 0;
		int _montageTest = 0;
	};
}
