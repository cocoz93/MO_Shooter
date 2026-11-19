#pragma once

#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"

#include "MOBSimpleAnimInstance.generated.h"

class ACharacter;
class UAnimSequence;
class UAnimMontage;

namespace mo { struct AnimPoseInput; struct BoneTransform; }

// 서버 구동 한 프레임의 결과 — 다리가 센다(9단계 5-a)
struct FMOBServerAnimResult
{
	int32 SetChange = 0;		// 0 그대로 · 1 클립 한 벌을 갈아 넣음 · -1 그 무기 한 벌이 없음
	int32 MontageCells = 0;		// 언리얼 경로(손가락 · 꼬임 뼈)에 넣은 몽타주 칸 수
	int32 MontageMisses = 0;	// 무기 번호 · 몽타주 자산이 없어 못 넣은 칸 수
	int32 SlotPasses = 0;		// 이번 갱신에서 엔진 SlotEvaluatePose 를 부른 끼움 자리 수(넣은 칸이 실제로 자세에 들어갔나)
	bool bMaskMissing = false;	// 윗몸 자리 뼈 가림(UpperBodyLowerBodySplitMask, D72 — 처음엔 UpperBodyMask)을 못 찾았다 — 윗몸 자리를 통째로 얹는다
	int32 WeaponNotifies = 0;	// 이번에 튼 무기 메시 몽타주 알림 수
	int32 WeaponPlaying = 0;	// 그중 튼 직후 붙은 무기 메시에 몽타주가 돌고 있던 수(알림이 실제로 무기를 움직였나)
	int32 SoundNotifies = 0;	// 이번에 튼 소리 알림 수
	int32 MontageRestarts = 0;	// 같은 자산 · 앞서지 않은 시각의 앞 칸이 있었지만, 틱 차이만큼 못 나가 새 칸으로 본 수(앞 상태와 틱이 벌어져 옛 칸이 빠지고 새 칸이 같은 자산으로 선 때)
	int32 StepStates = 0;		// 알림을 훑은 받은 상태 수(지난 프레임 뒤로 받은 상태 전부)
	int32 AdsLayers = 0;		// 받은 쌓기 층 중 조준 전환이라 같은 차례의 허리 전환으로 바꿔 튼 수(C-5)
	int32 CrouchLayers = 0;		// 받은 쌓기 층 중 앉은 전환이라 같은 차례의 허리 전환으로 바꿔 튼 수(D-3)
	int32 StanceLayers = 0;		// 받은 쌓기 층 중 서기 안 앉기 · 일어서기 전환이라 걷기 · 서기 자리로 돌린 수(D-3)
	int32 BadLayers = 0;		// 바꿔도 칸 밖이라 걷기 · 서기 자리로 돌린 층 수 — 0 이어야 한다
};

/**
 * 화면 시험용 — 상대 캐릭터를 Lyra 애니 구성 대신 소총 클립 5개를 섞는 단순 재생으로 움직인다.
 * 판정 후보 「서버와 화면이 같은 자세 계산을 쓴다」를 택했을 때 화면이 어떻게 보이는지 눈으로 보려고 넣었다. 판정에는 안 쓴다.
 * 섞는 입력은 서버가 아는 값(속도·몸 방향)뿐이다.
 * 일부러 안 넣은 것 = 위아래 조준 · 사격 반동 · 재장전 · 점프 · 사망 · 달리기 시작·멈춤 전용 클립 · 보폭 보정 · 발 디딤.
 * 명령줄 -MOBSimpleAnim 일 때만 켜진다.
 * -MOBSimpleAimJump 를 더하면 위아래 조준과 점프도 단순 계산으로 넣고, 걸음·섞는 때를 Lyra 원본 실행값에 맞춘다(2차 화면 시험, candidate1-arch-review.md 「B」).
 *   입력은 서버가 아는 값뿐이다 — 속도 · 몸 방향 · 시선 위아래 각 · 이동 입력이 있나 · 공중인가 · 위아래 속도 · 땅까지 거리 · 서 있을 때 캡슐이 바닥에서 뜬 거리.
 *   맞춘 값의 출처 = Lyra 원본을 같은 경로로 돌리며 프레임마다 뽑은 클립 가중치·시각·애니 변수(-MOBACProbe, animcmp_probe_lyra_probe.csv). 원본 애니 블루프린트를 열어 읽은 값이 아니다
 *   걸음 = 상대가 전부 Quinn(D32)이라 Lyra 가 Quinn 에 쓰는 여성 몸 소총 클립(MF_)을 쓴다. 플래그가 없으면 예전처럼 MM_ 클립이다
 *   위아래 조준 = Lyra 조준 보정 자산의 가운데 줄 세 자세(위 · 가운데 · 아래)를 각도로 섞어 얹는다. 좌우 줄은 안 쓴다 — 상대 몸 방향이 곧 시선 방향이다.
 *                이동 입력이 있는 동안은 Lyra 처럼 총을 내린 느슨한 조준 보정으로 옮겨 간다
 *   점프 = 뛰어오름 → 오르는 중 → 꼭대기 → 떨어지는 중 → 착지 클립. 착지 클립은 땅에 닿기 전에 땅까지 거리로 맞춰 보이고, 닿은 뒤엔 착지 충격 덧붙임을 얹는다
 *   서 있기 = 멈추고 7.35초(멈춤 클립 몫 1.35 + 지연 6) 가만히 서 있으면 대기 몸짓 클립을 한 번 보인다(Lyra 는 지연을 매번 무작위로 뽑는다 — 여기선 서버와 화면이 같아야 해 고정).
 *            서기 클립은 멈추고 1.35초에 처음부터 돈다(Lyra 가 서기 상태에 들어갈 때 처음부터 트는 것과 박자를 맞춘다)
 *   손 = 두 손을 IK 손 뼈(총 손잡이 자리)로 당긴다 — 조준 보정을 얹어도 왼손이 총에서 안 떨어진다
 *   발 = 땅에 있을 때 몸을 실제 바닥까지 내린다(메시 뿌리가 바닥보다 약 4cm 위라 그냥 두면 떠 보인다). 뜨고 0.2초 뒤 풀고, 닿고 0.1초 뒤 다시 내린다
 *   일부러 안 넣은 것(이 판에도) = 달리기 시작 · 멈춤 · 방향 전환 전용 클립, 달리며 몸 기울이기
 *   이 플래그가 없으면 예전과 같은 자세가 나온다(파일럿 1 기록을 다시 낼 수 있게). 11-06 첫 촬영의 단순 판(MM_ 클립 · 시간으로만 잇는 점프)은 이 판으로 바뀌었다
 * -MOBSimpleTransitions 를 -MOBSimpleAimJump 와 함께 주면 달리기 시작 · 멈춤 · 돌아서기 클립(네 방향 × 셋 = 12개)도 넣는다(3차 화면 시험, start-stop-pivot-test.md).
 *   입력은 서버가 아는 값뿐이다 — 위 입력에 가속(이동 입력)과 이동 상수(지면 마찰 · 제동 값)를 더한다.
 *   점프 쌓기(JumpStack)와 거리 곡선 표를 같이 쓴다. 보폭 보정 · 방향 틀기 · 달리며 몸 기울이기는 이 판에도 없다
 * -MOBServerAnim 이면(후보 1, D32) 남의 캐릭터는 프레임마다 시간을 쌓지 않고 서버가 보낸 상태를 그대로 쓴다.
 *   자세는 서버와 같은 Shared/AnimPose 한 벌로 계산해, 클립 파일의 27뼈(판정 뼈 · 그 조상 · 손 뼈)를 Evaluate 끝에서 덮는다. 손가락 · 꼬임 뼈는 아래 언리얼 경로가 맡는다
 *   언리얼 경로도 무기마다 클립 한 벌을 읽어 두고, 받은 무기 번호로 갈아 넣는다(9단계) — 안 그러면 권총을 든 몸에 소총 손가락이 붙는다
 */
USTRUCT()
struct FMOBSimpleAnimProxy : public FAnimInstanceProxy
{
	GENERATED_BODY()

	static constexpr int32 kClips = 5;
	static constexpr int32 kAim = 3;	// 위 · 가운데 · 아래
	static constexpr int32 kJump = 5;	// 뛰어오름 · 오르는 중 · 꼭대기 · 떨어지는 중 · 내려앉음
	static constexpr int32 kJumpStack = 4;	// 점프 전환이 겹칠 때 함께 섞이는 상태 수의 상한(Lyra 실행값에서 많아야 셋)
	static constexpr int32 kTransitions = 12;	// 네 방향 × 출발 · 멈춤 · 돌아서기(-MOBSimpleTransitions)
	static constexpr int32 kStackClips = kJump + kTransitions;	// 쌓기가 틀 수 있는 클립 수. 번호 = 점프 5개 뒤에 종류 × 4 + 방향
	static constexpr int32 kMontageKindCount = 12;	// 몽타주 종류 수(Shared/AnimClips.h MONTAGE_KIND_COUNT — cpp 에서 같은지 본다)
	static constexpr int32 kMontageCells = 3;		// 함께 도는 몽타주 칸 수의 상한(Shared/AnimPose.h kAnimMontageMax — cpp 에서 같은지 본다)

	// 몽타주 칸 하나가 앞 상태에서 이 상태까지 지나간 구간 — 알림을 틀 때 쓴다. From < 0 = 새로 생긴 칸(시각 0 부터 본다)
	struct FMontageStep
	{
		UAnimMontage* Montage = nullptr;
		float From = -1.f;
		float To = 0.f;
	};

	FMOBSimpleAnimProxy() {}
	FMOBSimpleAnimProxy(UAnimInstance* InAnimInstance) : FAnimInstanceProxy(InAnimInstance) {}

	void ResetPlayback();

	// 서버 구동(-MOBServerAnim) — 받은 상태와 계산해 둔 자세를 넣는다. 게임 스레드에서만 부른다. Local 은 BoneNames 차례의 뼈별 변환(부모 뼈 기준)
	// Weapon = 서버 무기 번호(EWeapon) — 지금 클립 한 벌과 다르면 그 무기 한 벌로 갈아 넣는다(Out.SetChange 0 = 그대로 · 1 = 갈아 넣음 · -1 = 그 무기 한 벌이 없음)
	// MontageWeapons = Input.montages 칸마다의 서버 무기 번호(-1 = 모름) — 언리얼 경로 몽타주는 그 무기 한 벌의 자산을 쓴다
	void SetServerFrame(const mo::AnimPoseInput& Input, int32 Weapon, const int32* MontageWeapons, const mo::BoneTransform* Local, int32 Count, const TArray<FName>& BoneNames, FMOBServerAnimResult& Out);
	// 알림용 — 받은 상태 하나의 몽타주 칸마다 앞 상태에서 지난 구간을 OutSteps 에 낸다. 받은 상태를 틱 차례로 하나씩 넣는다.
	// StateTick = 그 상태의 서버 틱(앞 상태와의 틱 차이로 칸이 이어졌나를 가른다). bContinue = false 면 앞 칸을 버리고 모두 새 칸으로 본다
	void StepMontages(const mo::AnimPoseInput& Input, uint32 StateTick, const int32* MontageWeapons, bool bContinue, FMOBServerAnimResult& Out, TArray<FMontageStep, TInlineAllocator<kMontageCells>>& OutSteps);
	// 마지막 Evaluate 가 SlotEvaluatePose 를 부른 자리 수 · 뼈 가림을 못 찾았나. 게임 스레드에서 자세 갱신을 마친 뒤에만 읽는다
	int32 GetSlotPasses() const { return SlotPasses; }
	bool IsMaskMissing() const { return bUpperMaskMissing; }

	// 대조 기록용 — 이번 프레임 자세에 쓴 가중치와 클립 시각을 그대로 내준다(언리얼 밖 계산의 입력)
	void GetState(float OutWeights[kClips], float OutTimes[kClips]) const;
	// 대조 기록용 — 클립 5개의 뼈별 변환(부모 뼈 기준)을 초당 SampleHz 개로 뽑아 BONE·CLIP·KEY 줄로 붙인다. KeepBones 에 든 뼈만 KEY 줄을 낸다
	// Montages(9단계 3-a) = 종류 차례(Shared/AnimClips.h EMontageKind)의 몽타주 — 줄마다 클립을 30개 뒤에 붙여 뽑고 MONTAGE · TRACK · MCURVE · SCURVE 줄을 낸다.
	// OverridePose = 쏜 뒤 윗몸 덮기 자세의 클립(OVERRIDE 줄). 둘 다 30개 판(-MOBSimpleAimJump -MOBSimpleTransitions)에서만 쓴다
	// AdsClips(C-3, D60) = 조준 클립 17개(서기 · 걷기 넷 · 전환 12). 같은 판에서 30개 바로 뒤(30~46)에 붙인다 — 몽타주 클립은 그 뒤로 밀린다
	// CrouchClips(D-3) = 앉기 클립 20개(서기 · 걷기 넷 · 전환 12 · 앉기 · 일어서기 전환 · 앉아 쏜 뒤 덮기 자세). 조준 클립 바로 뒤(47~66)
	void AppendClipDump(FString& Out, int32 SampleHz, const TSet<int32>& KeepBones, const TArray<const UAnimMontage*>& Montages, const UAnimSequence* OverridePose,
		const TArray<const UAnimSequence*>& AdsClips, const TArray<const UAnimSequence*>& CrouchClips) const;
	// 촬영 기록용 — 위아래 조준 · 점프에 쓴 입력과 상태
	void GetAimJump(float& OutPitch, int32& OutJumpClip, float& OutJumpWeight) const;
	// 대조 기록용 — 지금 재생 중인 클립을 Lyra 기록(-MOBACProbe)과 같은 「클립@시각*가중치;」 꼴로 붙인다
	void AppendPlayers(FString& Out) const;
	// 대조 기록용 — 자세 계산의 입력 중 가중치 · 시각을 뺀 나머지를 csv 칸으로 붙인다
	void AppendPoseState(FString& Out) const;
	// 대조 기록용 — 판 시작부터 매 프레임의 입력 원본(언리얼 밖 상태 계산의 입력, candidate1-impl-plan.md 3단계)
	const FString& GetMoveLog() const { return MoveLog; }

protected:
	virtual void PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) override;
	virtual bool Evaluate(FPoseContext& Output) override;

private:
	const UAnimSequence* Clips[kClips] = {};
	float Lengths[kClips] = {};
	float RootSpeeds[kClips] = {};
	TArray<float, TInlineAllocator<8>> StepTimes[kClips];	// 왼발 표시부터 차례로 다음 발 표시까지의 시각. 클립 끝을 넘어가면 길이를 더해 계속 커진다
	float Weights[kClips] = { 1.f, 0.f, 0.f, 0.f, 0.f };
	float Times[kClips] = {};
	int32 Step = 0;				// 지금 몇 번째 발걸음인가. 짝수 = 왼발에서 오른발로 가는 중. 클립마다 발자국 수가 달라 전체 진행률로는 발이 안 맞는다
	float StepFrac = 0.f;		// 그 발걸음 안의 진행률 0~1. 달리기 클립들이 함께 쓴다
	int32 StepCycle = 1;		// 달리기 클립 발자국 수의 최소공배수 — 이 주기로 돌려도 모든 클립의 발 순서가 그대로다
	float IdleTime = 0.f;

	// 위아래 조준 · 점프 · 대기 몸짓(-MOBSimpleAimJump)
	void UpdateIdleBreak(bool bStill, float DeltaSeconds);
	float FidgetAlpha() const;
	void UpdateJump(bool bFalling, bool bAccelerating, float VelZ, float TimeToApex, float GroundDistance, float DeltaSeconds);
	void PushJump(int32 Clip, float FadeSeconds);
	float LandTimeFromDistance(float GroundDistance) const;
	void ApplyJump(FPoseContext& Output) const;
	void ApplyRecovery(FPoseContext& Output) const;
	void ApplyAim(FPoseContext& Output) const;
	void AccumulateAim(FPoseContext& Output, const UAnimSequence* const Set[kAim], const float SetPitches[kAim], int32 AdditiveType, float Weight) const;
	bool bAimJump = false;
	const UAnimSequence* AimClips[kAim] = {};		// 허리 사격 조준 보정 — 서 있을 때
	float AimPitches[kAim] = { 90.f, 0.f, -90.f };	// 각 자세가 맡는 위아래 각 — 조준 보정 자산에서 읽어 덮는다
	int32 AimAdditiveType = 0;					// EAdditiveAnimationType — 가운데 자세 것을 쓴다
	const UAnimSequence* ReadyAimClips[kAim] = {};	// 느슨한 조준 보정 — 이동 입력이 있을 때
	float ReadyAimPitches[kAim] = { 90.f, 0.f, -90.f };
	int32 ReadyAimAdditiveType = 0;
	float AimBlend = 1.f;		// 1 = 허리 사격 조준만, 0 = 느슨한 조준만
	float Pitch = 0.f;

	const UAnimSequence* JumpClips[kStackClips] = {};
	float JumpLengths[kStackClips] = {};
	TArray<float> LandDistances;	// 착지 클립의 땅까지 거리 곡선을 일정 간격으로 뽑은 값. 비면 거리로 못 맞춘다
	// 점프 쪽 상태를 쌓아 섞는다 — 새 상태가 겹쳐 들어오며 아래 것들을 덮는다(언리얼 상태 기계의 전환 섞기를 직접 한 것). Clip -1 = 걷기·서기
	struct FJumpLayer
	{
		int32 Clip = -1;
		float Time = 0.f;
		float Fade = 1.f;			// 0~1 덮은 정도
		float FadeSeconds = 0.f;
	};
	FJumpLayer JumpStack[kJumpStack];
	int32 JumpCount = 0;			// 0 = 점프 쪽 없음(걷기·서기만)
	void JumpWeights(float Out[kJumpStack]) const;
	int32 JumpClip = -1;		// 기록용 — 맨 위 점프 클립. -1 = 없음
	float JumpWeight = 0.f;		// 기록용 — 걷기·서기가 아닌 몫
	bool bWasFalling = false;

	// 출발 · 멈춤 · 돌아서기(-MOBSimpleTransitions) — 점프 쌓기 위에 같은 방식으로 얹는다
	enum class EMoveState : uint8 { Idle, Start, Cycle, Stop, Pivot, Air };
	struct FStepMarker
	{
		float Time = 0.f;
		bool bLeft = false;
	};
	void UpdateTransitions(bool bFalling, bool bAccelerating, const FVector& LocalVelocity, const FVector& LocalAcceleration, const class UCharacterMovementComponent* Move, float DeltaSeconds);
	void EnterTransition(EMoveState State, int32 Direction, float FadeSeconds);
	void EnterLocomotion(EMoveState State, float FadeSeconds);
	float TransitionClipTime(const FJumpLayer& Layer, float DeltaSeconds) const;
	float TimeFromDistance(int32 Transition, float Value) const;
	float DistanceAtTime(int32 Transition, float Time) const;
	int32 StepMarkerAt(int32 Transition, float Time) const;
	bool FollowStepsOfTransition(float DeltaSeconds);
	bool HasJumpLayer() const;
	static const TCHAR* MoveStateName(EMoveState State);
	int32 FollowClip = INDEX_NONE;	// 달리기 클립이 발을 따라가는 나가는 전환 클립
	int32 FollowBaseStep = 0;		// 따라가기 시작할 때의 걸음과
	int32 FollowBaseMarker = 0;		// 그때 그 클립의 발 표시 번호
	bool bTransitions = false;
	EMoveState MoveState = EMoveState::Idle;
	int32 MoveDirection = 0;		// 지금 틀고 있는 전환 클립의 방향
	FVector2D PivotAccelDirection = FVector2D::ZeroVector;	// 돌아서기에 들어갈 때의 가속 방향 — 다시 뒤집혔는지 본다
	float FrameTravel = 0.f;		// 이번 프레임에 땅 위로 움직인 거리(cm)
	float StopDistance = 0.f;		// 지금 속도로 멈출 때까지 남은 거리(cm)
	float PivotDistance = 0.f;		// 방향이 바뀌는 지점까지 남은 거리(cm)
	bool bBeforePivot = false;		// 아직 옛 방향으로 밀려 가는 중인가
	TArray<float> TransitionDistances[kTransitions];	// 클립의 Distance 곡선을 일정 간격으로 뽑은 값
	TArray<FStepMarker, TInlineAllocator<8>> TransitionMarkers[kTransitions];	// 클립의 발 표시 — 달리기로 넘어갈 때 발을 맞춘다
	int32 StackMax = 0;				// 시험 기록 — 쌓기에 함께 올라온 상태 수의 최대
	int32 StackOverflows = 0;		// 시험 기록 — 자리가 없어 맨 아래를 버린 횟수
	FString MoveLog;				// 시험 기록 — 입력 원본
	int32 ResetGeneration = 0;		// 시험 기록 — 판 시작(ResetPlayback) 횟수
	int32 MoveSeq = 0;				// 시험 기록 — 판 시작 뒤 지난 프레임 수

	const UAnimSequence* RecoveryClip = nullptr;	// 내려앉은 뒤 얹는 착지 충격 덧붙임
	float RecoveryLength = 0.f;
	int32 RecoveryAdditiveType = 0;
	float RecoveryTime = -1.f;	// 땅에 닿은 뒤 지난 시간. -1 = 안 얹음

	const UAnimSequence* FidgetClip = nullptr;	// 서 있을 때 대기 몸짓
	float FidgetLength = 0.f;
	float StandTime = 0.f;		// 가만히 선 채 지난 시간
	float FidgetTime = -1.f;	// -1 = 안 보임
	bool bFidgetLeaving = false;	// 움직이기 시작했다 — 서기 몫이 다 빠지면 끝낸다
	bool bWasAccelerating = false;	// 지난 프레임에 이동 입력이 있었나 — 놓는 순간 서기 클립 시각을 옮긴다

	// 무기별 클립 한 벌 — 위 멤버 중 읽은 클립에서 나오는 것의 사본(느슨한 조준은 무기와 상관없이 하나라 뺀다).
	// 서버 구동이면 무기마다 한 벌을 묶어 두고 무기가 바뀌면 통째로 갈아 넣는다. 아니면 한 벌뿐이다
	struct FClipSet
	{
		bool bLoaded = false;
		const UAnimSequence* Clips[kClips] = {};
		float Lengths[kClips] = {};
		float RootSpeeds[kClips] = {};
		TArray<float, TInlineAllocator<8>> StepTimes[kClips];
		int32 StepCycle = 1;
		const UAnimSequence* AimClips[kAim] = {};
		float AimPitches[kAim] = {};
		int32 AimAdditiveType = 0;
		const UAnimSequence* JumpClips[kStackClips] = {};
		float JumpLengths[kStackClips] = {};
		TArray<float> LandDistances;
		TArray<float> TransitionDistances[kTransitions];
		TArray<FStepMarker, TInlineAllocator<8>> TransitionMarkers[kTransitions];
		const UAnimSequence* RecoveryClip = nullptr;
		float RecoveryLength = 0.f;
		int32 RecoveryAdditiveType = 0;
		const UAnimSequence* FidgetClip = nullptr;
		float FidgetLength = 0.f;
		UAnimMontage* Montages[kMontageKindCount] = {};	// 종류 차례(EMontageKind). 서버 구동일 때만 있다
	};
	void BindSet(const class UMOBSimpleAnimInstance& Instance, int32 SetIndex);	// 읽은 한 벌을 위 멤버에 묶는다(로그 포함)
	void SaveSet(FClipSet& Out) const;
	void UseSet(const FClipSet& In);
	TArray<FClipSet> ClipSets;		// 차례 = 인스턴스의 LoadedSets
	int32 ActiveSet = INDEX_NONE;	// 지금 위 멤버에 든 한 벌
	int32 WantedSet = INDEX_NONE;	// 묶기 전에 받은 서버 무기 — 처음 묶을 때 이 한 벌부터 넣는다

	// 서버 구동(-MOBServerAnim)
	void ApplyServerBones(FPoseContext& Output);
	bool bServerDriven = false;
	TArray<FName> ServerBoneNames;		// 클립 파일의 뼈 이름. 차례 = Shared 결과의 차례
	TArray<FTransform> ServerLocal;		// 이번에 덮을 뼈별 변환(부모 뼈 기준)
	TArray<int32, TInlineAllocator<32>> ServerBoneMap;	// 뼈 차례 → 압축 뼈 번호. -1 = 이 메시에 없음
	uint16 ServerBoneSerial = 0;		// ServerBoneMap 을 만들 때의 뼈 목록 번호 — LOD 가 바뀌면 다시 만든다

	// 몽타주(9단계 5-a) — 받은 칸을 엔진 몽타주 평가 자료로 넣고 끼움 자리 넷을 엔진 SlotEvaluatePose 로 얹는다(손가락 · 꼬임 뼈 몫).
	// 쏜 뒤 윗몸 덮기는 안 넣는다(제 판단, 재개점 설계) — 판정 뼈는 어차피 서버 자세가 덮고, 빠지는 것은 덮기 동안의 손가락 · 꼬임 뼈 몫뿐이다
	// 차례는 서버와 같은 Lyra 그래프 차례(D72) — bAfterAim 거짓 = 윗몸 섞기 · 사격 덧붙임(조준 앞), 참 = 맞은 쪽 반응 · 통째(조준 뒤)
	void ApplyMontages(FPoseContext& Output, bool bAfterAim);
	struct FMontageCell
	{
		UAnimMontage* Montage = nullptr;
		float Position = 0.f;
		float Weight = 0.f;
	};
	FMontageCell MontageCells[kMontageCells];
	int32 MontageCellCount = 0;
	FMontageCell NotifyCells[kMontageCells];	// 알림을 마지막으로 훑은 상태의 칸(MontageCells 는 보여 줄 상태의 칸이라 따로 둔다)
	int32 NotifyCellCount = 0;
	uint32 NotifyTick = 0;			// NotifyCells 를 받은 상태의 서버 틱
	TArray<FPerBoneBlendWeight> UpperMaskWeights;	// 윗몸 자리 뼈 가림(UpperBodyLowerBodySplitMask, D72) — 압축 뼈 차례
	uint16 UpperMaskSerial = 0;		// UpperMaskWeights 를 만들 때의 뼈 목록 번호
	bool bUpperMaskMissing = false;
	int32 SlotPasses = 0;			// 마지막 Evaluate 가 SlotEvaluatePose 를 부른 자리 수

	// 손 붙이기 · 발 디딤 내림(-MOBSimpleAimJump)
	void ApplyHandIK(FPoseContext& Output) const;
	void ApplyPlant(FPoseContext& Output) const;
	float PlantDrop = 0.f;		// 마지막으로 잰 메시 뿌리 ~ 바닥 거리(cm). 뜬 직후엔 이 값을 그대로 쓴다
	FVector PlantOffset = FVector::ZeroVector;	// 이번 프레임에 골반을 옮길 양(메시 기준). 0 = 안 내림
	float GroundTime = 0.f;		// 땅에 닿은 뒤 지난 시간
	float AirTime = 0.f;		// 땅을 떠난 뒤 지난 시간
	bool bPlantLogged = false;	// 이번 착지에서 잰 거리를 기록에 남겼나
};

// 무기 한 벌의 읽은 클립. 프록시는 맨 포인터로 읽으므로 인스턴스가 잡아 둔다(가비지 수거 방지)
USTRUCT()
struct FMOBLoadedClipSet
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	TArray<TObjectPtr<UAnimSequence>> Clips;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UAnimSequence>> Aim;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UAnimSequence>> Jump;
	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> Recovery;
	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> Fidget;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UAnimMontage>> Montages;	// 종류 차례(EMontageKind) — 서버 구동일 때만 읽는다(9단계 5-a)
	float AimPitches[FMOBSimpleAnimProxy::kAim] = { 90.f, 0.f, -90.f };
	const TCHAR* Key = nullptr;		// 클립 이름 표의 무기(= 클립 파일 묶음 이름). nullptr = 이 무기의 이름 표가 없어 못 읽음
};

UCLASS(Transient, NotBlueprintable)
class UMOBSimpleAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

	friend struct FMOBSimpleAnimProxy;

public:
	void ResetPlayback();

	// 대조 기록용 — 프록시는 이 클래스 안에서만 꺼낼 수 있어 여기를 거친다
	void GetPlaybackState(float OutWeights[FMOBSimpleAnimProxy::kClips], float OutTimes[FMOBSimpleAnimProxy::kClips]);
	bool DumpClips(int32 SampleHz, FString& Out);
	void GetAimJump(float& OutPitch, int32& OutJumpClip, float& OutJumpWeight);
	void GetPlayers(FString& Out);
	void GetPoseState(FString& Out);
	void GetMoveLog(FString& Out);
	// 받은 칸으로 자세 입력을 넣는다
	void SetServerFrame(const mo::AnimPoseInput& Input, int32 Weapon, const int32* MontageWeapons, const mo::BoneTransform* Local, int32 Count, const TArray<FName>& BoneNames, FMOBServerAnimResult& Out);
	// 받은 상태 하나의 몽타주 칸이 지난 구간에서 무기 몽타주 · 소리 알림을 튼다(다른 알림은 안 튼다 — 게임 사건이 남의 캐릭터 능력을 켤 수 있다)
	void StepServerMontages(const mo::AnimPoseInput& Input, uint32 StateTick, const int32* MontageWeapons, bool bContinue, FMOBServerAnimResult& Out);
	// 자세 갱신을 마친 뒤 마지막 Evaluate 의 결과를 Out 에 옮긴다
	void ReadEvaluateResult(FMOBServerAnimResult& Out);

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;

private:
	void LoadClips();

	// 프록시는 맨 포인터로 읽으므로 여기서 잡아 둔다(가비지 수거 방지)
	// 무기별 한 벌 — 서버 구동(-MOBServerAnim)이면 서버 무기 번호(EWeapon) 차례로 무기마다 한 벌, 아니면 명령줄 무기 한 벌뿐
	UPROPERTY(Transient)
	TArray<FMOBLoadedClipSet> LoadedSets;
	int32 PrimarySet = 0;			// 처음 묶을 한 벌 = 명령줄 무기(-MOBACWeapon, 없으면 소총)
	UPROPERTY(Transient)
	TArray<TObjectPtr<UAnimSequence>> LoadedReadyAim;	// 느슨한 조준 — 무기와 상관없이 하나
	float ReadyAimPitches[FMOBSimpleAnimProxy::kAim] = { 90.f, 0.f, -90.f };

	bool bClipsLoaded = false;
	bool bAimJump = false;
	bool bTransitions = false;
	const TCHAR* WeaponKey = TEXT("rifle");	// 읽은 클립의 무기(-MOBACWeapon). 클립 자료 뽑기의 WEAPON 줄 = 클립 파일 묶음 이름
};

// 캐릭터의 본체 메시를 단순 재생으로 바꾼다. 눈에 보이는 몸(SKM_Quinn)이 본체(SKM_Quinn_Invis) 자세를 따라가는 것은 촬영 화면으로 확인했다
void MOBApplySimpleAnim(ACharacter* Character);

// 재생 상태를 처음으로 되돌린다. 촬영에서 판끼리 시작을 맞추려고 쓴다 — 같은 입력이면 같은 자세가 나오는지 재려면 시작이 같아야 한다
void MOBResetSimpleAnim(ACharacter* Character);

// 대조 기록용(candidate1-pilot-plan.md 파일럿 1). 단순 재생이 아니면 false
bool MOBGetSimpleAnimState(const ACharacter* Character, float OutWeights[FMOBSimpleAnimProxy::kClips], float OutTimes[FMOBSimpleAnimProxy::kClips]);
bool MOBDumpSimpleClips(const ACharacter* Character, int32 SampleHz, FString& Out);

// 촬영 기록용 — 단순 재생이 쓴 위아래 각 · 점프 클립(-1 = 없음) · 점프 가중치. 단순 재생이 아니면 false
bool MOBGetSimpleAimJump(const ACharacter* Character, float& OutPitch, int32& OutJumpClip, float& OutJumpWeight);

// 대조 기록용 — 단순 재생이 지금 틀고 있는 클립. 단순 재생이 아니면 false
bool MOBGetSimplePlayers(const ACharacter* Character, FString& Out);

// 대조 기록용 — 자세 계산의 입력 중 가중치 · 시각을 뺀 나머지를 csv 칸으로 붙인다(Shared/AnimPose 의 AnimPoseInput 과 같은 칸). 단순 재생이 아니면 기본값
void MOBAppendSimplePoseState(const ACharacter* Character, FString& Out);

// 대조 기록용 — 판 시작부터 매 프레임의 입력 원본. 단순 재생이 아니면 false
bool MOBGetSimpleMoveLog(const ACharacter* Character, FString& Out);

// 서버 구동(-MOBServerAnim) — 받은 상태와 계산해 둔 자세를 넣고, 이번 프레임 화면에 바로 반영한다. 단순 재생이 아니면 false
// Weapon = 서버 무기 번호(EWeapon). MontageWeapons = Input.montages 칸마다의 서버 무기 번호(-1 = 모름). OutResult = 한 벌 갈아 넣기 · 몽타주 칸 · 끼움 자리 평가 수(처음부터 다시 센다, 알림 수는 MOBStepServerMontages 가 더한다)
bool MOBSetServerAnim(ACharacter* Character, const mo::AnimPoseInput& Input, int32 Weapon, const int32* MontageWeapons, const mo::BoneTransform* Local, int32 Count, const TArray<FName>& BoneNames, FMOBServerAnimResult& OutResult);

// 서버 구동 — 받은 상태 하나로 몽타주 알림을 튼다. 지난 프레임 뒤로 받은 상태를 틱 차례로 하나씩 넣는다(멈칫한 프레임 사이에 생겼다 진 칸도 튼다).
// bContinue = false 면 앞 상태를 버린다(처음 · 죽었다 살아남). InOutResult 의 알림 수에 더한다. 단순 재생이 아니면 false
bool MOBStepServerMontages(ACharacter* Character, const mo::AnimPoseInput& Input, uint32 StateTick, const int32* MontageWeapons, bool bContinue, FMOBServerAnimResult& InOutResult);

// 몸 부품을 바꾼다. PartClassPath = 부품 블루프린트 클래스 경로. 컴포넌트나 부품을 못 찾으면 false
bool MOBSetBodyPart(ACharacter* Character, const TCHAR* PartClassPath);

// 촬영 대조용 — 단순 재생의 대기 몸짓 지연과, 멈춘 뒤 몸짓까지의 시간(멈춤 클립 몫 + 지연). Lyra 쪽 무작위 지연을 이 값으로 덮어 두 화면을 맞춘다
void MOBGetSimpleIdleBreak(float& OutDelay, float& OutAfterStop);
