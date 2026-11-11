#pragma once

#include "AnimClips.h"
#include "AnimPose.h"
#include "GameProtocol.h"

#include <cstdint>
#include <vector>

// 입력 → 애니 상태. 서버가 틱마다 돌리고 결과를 되감기 기록과 패킷에 싣는다(D32).
// - 옮긴 원본 = MOBSimpleAnimInstance.cpp 의 UpdateIdleBreak ~ UpdateJump 와 PreUpdate 의 입력 처리.
//   같은 입력에 언리얼과 같은 상태가 나오는지는 Tools/AnimCheck state + check_pose.py 로 대조한다.
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
		// 클립 자료와 묶음을 고른다. 클립 수가 다르거나 발 표시 · 거리 곡선이 없으면 false
		bool Init(const CAnimClips& clips, const char* bundleName);
		void Reset();
		void Update(const AnimMoveInput& input);
		void ToPoseInput(AnimPoseInput& out) const;

		// 패킷 · 되감기 기록에 싣는 형식. 서버 판정과 화면이 둘 다 이 값으로 자세를 만들어야 양쪽이 같은 수를 본다(D32 · D35)
		void ToWire(wire::AnimStateWire& out) const;
		// 받은 상태 → 자세 입력. 이 객체의 표(클립 길이 · 발 표시)만 읽고 재생 상태는 안 건드린다. pitch 는 PlayerState 에 이미 있는 값이다
		void FromWire(const wire::AnimStateWire& in, float pitch, AnimPoseInput& out) const;

		int StackOverflows() const { return _stackOverflows; }

		// 대조 시험의 일부러 틀림용 — 제동 감속을 배율만큼 바꾼다
		void SetBrakingScaleForTest(float scale) { _brakingDecelerationWalking *= scale; }

	private:
		enum class EMoveState : uint8_t { Idle, Start, Cycle, Stop, Pivot, Air };

		struct Layer
		{
			int clip = -1;
			float time = 0.f;
			float fade = 1.f;
			float fadeSeconds = 0.f;
		};

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

		// 클립에서 읽어 둔 값
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
	};
}
