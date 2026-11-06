#pragma once

#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"

#include "MOBSimpleAnimInstance.generated.h"

class ACharacter;
class UAnimSequence;

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
 */
USTRUCT()
struct FMOBSimpleAnimProxy : public FAnimInstanceProxy
{
	GENERATED_BODY()

	static constexpr int32 kClips = 5;
	static constexpr int32 kAim = 3;	// 위 · 가운데 · 아래
	static constexpr int32 kJump = 5;	// 뛰어오름 · 오르는 중 · 꼭대기 · 떨어지는 중 · 내려앉음
	static constexpr int32 kJumpStack = 4;	// 점프 전환이 겹칠 때 함께 섞이는 상태 수의 상한(Lyra 실행값에서 많아야 셋)

	FMOBSimpleAnimProxy() {}
	FMOBSimpleAnimProxy(UAnimInstance* InAnimInstance) : FAnimInstanceProxy(InAnimInstance) {}

	void ResetPlayback();

	// 대조 기록용 — 이번 프레임 자세에 쓴 가중치와 클립 시각을 그대로 내준다(언리얼 밖 계산의 입력)
	void GetState(float OutWeights[kClips], float OutTimes[kClips]) const;
	// 대조 기록용 — 클립 5개의 뼈별 변환(부모 뼈 기준)을 초당 SampleHz 개로 뽑아 BONE·CLIP·KEY 줄로 붙인다. KeepBones 에 든 뼈만 KEY 줄을 낸다
	void AppendClipDump(FString& Out, int32 SampleHz, const TSet<int32>& KeepBones) const;
	// 촬영 기록용 — 위아래 조준 · 점프에 쓴 입력과 상태
	void GetAimJump(float& OutPitch, int32& OutJumpClip, float& OutJumpWeight) const;

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

	const UAnimSequence* JumpClips[kJump] = {};
	float JumpLengths[kJump] = {};
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

	// 손 붙이기 · 발 디딤 내림(-MOBSimpleAimJump)
	void ApplyHandIK(FPoseContext& Output) const;
	void ApplyPlant(FPoseContext& Output) const;
	float PlantDrop = 0.f;		// 마지막으로 잰 메시 뿌리 ~ 바닥 거리(cm). 뜬 직후엔 이 값을 그대로 쓴다
	FVector PlantOffset = FVector::ZeroVector;	// 이번 프레임에 골반을 옮길 양(메시 기준). 0 = 안 내림
	float GroundTime = 0.f;		// 땅에 닿은 뒤 지난 시간
	float AirTime = 0.f;		// 땅을 떠난 뒤 지난 시간
	bool bPlantLogged = false;	// 이번 착지에서 잰 거리를 기록에 남겼나
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

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;

private:
	void LoadClips();

	// 프록시는 맨 포인터로 읽으므로 여기서 잡아 둔다(가비지 수거 방지)
	UPROPERTY(Transient)
	TArray<TObjectPtr<UAnimSequence>> LoadedClips;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UAnimSequence>> LoadedAim;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UAnimSequence>> LoadedReadyAim;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UAnimSequence>> LoadedJump;
	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> LoadedRecovery;
	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> LoadedFidget;
	float AimPitches[FMOBSimpleAnimProxy::kAim] = { 90.f, 0.f, -90.f };
	float ReadyAimPitches[FMOBSimpleAnimProxy::kAim] = { 90.f, 0.f, -90.f };

	bool bClipsLoaded = false;
	bool bAimJump = false;
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

// 촬영 대조용 — 단순 재생의 대기 몸짓 지연과, 멈춘 뒤 몸짓까지의 시간(멈춤 클립 몫 + 지연). Lyra 쪽 무작위 지연을 이 값으로 덮어 두 화면을 맞춘다
void MOBGetSimpleIdleBreak(float& OutDelay, float& OutAfterStop);
