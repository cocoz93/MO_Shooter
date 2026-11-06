#include "MOBSimpleAnimInstance.h"

#include "Animation/AnimNodeBase.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimationPoseData.h"
#include "Animation/BlendSpace.h"
#include "Animation/BoneReference.h"
#include "AnimationRuntime.h"
#include "BonePose.h"
#include "TwoBoneIK.h"
#include "CollisionQueryParams.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MOBSimpleAnimInstance)

DEFINE_LOG_CATEGORY_STATIC(LogMOBSimple, Log, All);

namespace
{
	enum EClip { CLIP_IDLE, CLIP_FWD, CLIP_BWD, CLIP_LEFT, CLIP_RIGHT };

	const TCHAR* const CLIP_NAMES[FMOBSimpleAnimProxy::kClips] = {
		TEXT("MM_Rifle_Idle_Hipfire"),
		TEXT("MM_Rifle_Jog_Fwd"),
		TEXT("MM_Rifle_Jog_Bwd"),
		TEXT("MM_Rifle_Jog_Left"),
		TEXT("MM_Rifle_Jog_Right"),
	};
	// -MOBSimpleAimJump — Lyra 가 Quinn 몸에 쓰는 여성 몸 소총 층(ABP_RifleAnimLayers_Feminine)의 같은 자리 클립. 실행값에서 이 이름들이 재생되는 것을 봤다
	const TCHAR* const CLIP_NAMES_FEMININE[FMOBSimpleAnimProxy::kClips] = {
		TEXT("MF_Rifle_Idle_Hipfire"),
		TEXT("MF_Rifle_Jog_Fwd"),
		TEXT("MF_Rifle_Jog_Bwd"),
		TEXT("MF_Rifle_Jog_Left"),
		TEXT("MF_Rifle_Jog_Right"),
	};
	const TCHAR* ClipName(bool bAimJump, int32 I)
	{
		return bAimJump ? CLIP_NAMES_FEMININE[I] : CLIP_NAMES[I];
	}

	// 시험용 제안값 — Lyra 원본에서 뽑은 값이 아니다. 서기와 달리기를 섞는 구간과 동작 전환의 부드러움을 정한다
	constexpr float MOVE_FULL_SPEED = 300.f;	// 이 속도(cm/s)부터 달리기 클립 100%. Lyra 달리기 최고 속도의 절반쯤
	constexpr float BLEND_SECONDS = 0.1f;		// 가중치가 목표로 다가가는 시간 상수. 0.1이면 약 0.3초에 전환이 끝난다
	constexpr float MIN_PLAY_RATE = 0.25f;
	constexpr float MAX_PLAY_RATE = 1.5f;

	// 위아래 조준 — Lyra 소총 허리 사격 대기의 조준 보정. 가운데 줄(C = 정면) 세 자세만 쓴다
	enum EAim { AIM_UP, AIM_CENTER, AIM_DOWN };
	const TCHAR* const AIM_NAMES[FMOBSimpleAnimProxy::kAim] = {
		TEXT("MM_Rifle_Idle_Hipfire_AO_CU"),
		TEXT("MM_Rifle_Idle_Hipfire_AO_CC"),
		TEXT("MM_Rifle_Idle_Hipfire_AO_CD"),
	};
	const TCHAR* const AIM_OFFSET_PATH = TEXT("/Game/Characters/Heroes/Mannequin/Animations/AimOffsets/AO_MM_Rifle_Idle_Hipfire.AO_MM_Rifle_Idle_Hipfire");
	// 느슨한 조준 — Lyra 는 이동 입력이 있는 동안 위 조준 보정에서 이것으로 옮겨 간다(실행값: 달리는 동안 이 자산 가중치 1)
	const TCHAR* const READY_AIM_NAMES[FMOBSimpleAnimProxy::kAim] = {
		TEXT("MM_Unarmed_Idle_Ready_AO_CU"),
		TEXT("MM_Unarmed_Idle_Ready_AO_CC"),
		TEXT("MM_Unarmed_Idle_Ready_AO_CD"),
	};
	const TCHAR* const READY_AIM_OFFSET_PATH = TEXT("/Game/Characters/Heroes/Mannequin/Animations/AimOffsets/AO_MM_Unarmed_Idle_Ready.AO_MM_Unarmed_Idle_Ready");

	// 점프 — Lyra 소총 점프 클립을 이 차례로 잇는다
	enum EJump { JUMP_START, JUMP_START_LOOP, JUMP_APEX, JUMP_FALL_LOOP, JUMP_LAND };
	constexpr int32 JUMP_NONE = -1;	// 쌓기에서 걷기·서기 자리
	const TCHAR* const JUMP_NAMES[FMOBSimpleAnimProxy::kJump] = {
		TEXT("MM_Rifle_Jump_Start"),
		TEXT("MM_Rifle_Jump_Start_Loop"),
		TEXT("MM_Rifle_Jump_Apex"),
		TEXT("MM_Rifle_Jump_Fall_Loop"),
		TEXT("MM_Rifle_Jump_Fall_Land"),
	};
	const TCHAR* const RECOVERY_NAME = TEXT("MM_Rifle_Jump_RecoveryAdditive");
	const TCHAR* const FIDGET_NAME = TEXT("Mf_Rifle_IdleBreak_Fidget");
	const FName LAND_DISTANCE_CURVE(TEXT("GroundDistance"));	// Lyra 가 착지 클립을 맞출 때 쓰는 곡선 이름

	// 아래는 Lyra 원본 실행값에서 읽은 값이다(animcmp_probe_lyra_probe.csv, 11-06 · 60프레임 · 제자리 점프와 달리며 점프가 같은 값).
	// 애니 블루프린트를 열어 본 값이 아니라 프레임별 클립 가중치·시각·변수에서 읽은 것 — 「확인」 옆 숫자가 그 근거다
	constexpr float AIM_BLEND_SPEED = 10.f;		// 조준 보정 바꿈 빠르기(FInterpTo). 확인: 이동 입력이 생기고 0.1초 뒤 가중치 1 → 0.334, 멈추고 2프레임 뒤 0 → 0.305
	constexpr float JUMP_BLEND_SECONDS = 0.1f;	// 걷기 → 뛰어오름, 오르는 중 → 꼭대기, 꼭대기 → 떨어지는 중. 확인: 가중치 0.07 · 0.26 · 0.50 · 0.74 · 0.93 = 부드러운 곡선
	constexpr float APEX_LEAD_SECONDS = 0.25f;	// 꼭대기까지 남은 시간이 이보다 짧으면 꼭대기 클립으로. 확인: 0.260 에선 그대로, 0.244 에서 넘어감
	constexpr float APEX_EXIT_REMAINING = 0.12f;	// 꼭대기 클립이 이만큼 못 남으면 떨어지는 중으로. 확인: 0.133 남았을 땐 그대로, 0.117 남았을 때 넘어감
	constexpr float LAND_FADE_SECONDS = 0.3f;	// 떨어지는 중 → 착지 클립(부드러운 곡선). 확인: 2 · 8 · 13프레임 뒤 0.03 · 0.42 · 0.78
	// 착지 클립으로 넘어가는 문턱 = 착지 클립 곡선의 시작 거리(203cm). Lyra 의 문턱은 모른다 — 실행값에선 83.5cm 보다 크다는 것만 안다
	constexpr float LANDED_STAND_SECONDS = 0.15f;	// 땅에 닿은 뒤 서기로(부드러운 곡선, 이동 입력 없음). 확인: 2 · 5 · 8프레임 뒤 0.13 · 0.58 · 0.97
	constexpr float LANDED_MOVE_SECONDS = 0.2f;		// 땅에 닿은 뒤 달리기로(이동 입력 있음). 확인: 2 · 5 · 8 · 11프레임 뒤 0.07 · 0.38 · 0.74 · 0.98
	constexpr float RECOVERY_WEIGHT = 0.65f;	// 착지 충격 덧붙임의 세기. 확인: 0.5초 떨어진 점프에서 0.65. 더 짧게 떨어질 때의 값은 모른다
	constexpr float RECOVERY_BLEND_SECONDS = 0.2f;	// 넣을 때와 클립 끝에서 뺄 때(부드러운 곡선). 확인: 넣기 0.05 · 0.24 · 0.48 · 0.64 / 빼기 0.59 · 0.37 · 0.12 (3프레임 간격, 0.65 기준)
	constexpr float IDLE_BREAK_DELAY = 6.f;		// 서기 상태에서 이만큼 지나면 대기 몸짓. Lyra 는 지연 변수를 매번 무작위로 뽑는다(실행값 6 · 7 · 10 · 11 · 15초, 첫 지연도 판마다 다름) — 서버와 화면이 같아야 해 고정.
												// 6 = 처음 본 값. 시험 경로의 서 있는 구간(약 8초) 안에 몸짓이 한 번 보인다. 촬영 대조 때는 Lyra 쪽 변수를 이 값으로 덮는다(MOBGetSimpleIdleBreak)
	constexpr float STOP_CLIP_SECONDS = 1.35f;	// Lyra 는 멈춤 클립이 끝나 서기 상태에 들어가야 위 시계를 센다. 확인: 입력을 놓고 1.40초 뒤 · 첫 판 시작 1.33초 뒤부터 셈.
												// 단순 계산엔 멈춤 클립이 없어 멈춘 뒤 이만큼 기다렸다 센다(착지 뒤엔 Lyra 가 바로 세지만 여기선 똑같이 기다린다)
	constexpr float IDLE_BREAK_BLEND = 0.4f;	// 대기 몸짓 넣고 빼기(부드러운 곡선). 확인: 0.133초 뒤 0.26 · 0.267초 뒤 0.74
	// 서기 클립도 Lyra 는 서기 상태에 들어갈 때 처음부터 튼다. 확인: 입력을 놓고 1.37초 뒤 서기 클립 0초에서 시작(stop_end).
	// 단순 계산은 입력을 놓는 순간(서기 몫이 아직 0 근처) 서기 시각을 「길이 - STOP_CLIP_SECONDS」로 옮겨, 그만큼 뒤에 0초를 지나게 한다 — 도중에 자세가 튀지 않는다
	// 발 디딤 내림 — Lyra 는 땅에 있을 때 몸을 실제 바닥까지 내린다. 확인: 서 있는 동안 골반·발·IK 발 뼈가 단순 계산보다 3.97 · 4.15cm 아래(머리·손도 같음), 공중에선 0.
	// IK 손 뼈는 안 내려간다(손 - IK 손 = 4.0). 그 크기 = 메시 뿌리 높이(캡슐 바닥 위 2cm) + 캡슐이 바닥에서 뜬 거리로 짐작한다 — Lyra 의 어느 노드가 하는지는 열어 보지 않았다
	constexpr float PLANT_RELEASE_SECONDS = 0.2f;	// 땅을 떠나고 이만큼은 그대로 내려 둔다. 확인: 제자리·달리며 점프 모두 뜬 뒤 12프레임째 골반 차이 -3.0 → 0 으로 한 번에 끊김
	constexpr float PLANT_LAND_DELAY = 0.1f;		// 땅에 닿고 이만큼 지나야 다시 내린다. 확인: 닿고 5프레임까지 골반 차이 0.3cm 안, 6프레임째 -2.7
	constexpr float PLANT_TIME_SLACK = 0.001f;		// 프레임 시간을 더해 가며 비교할 때 반올림 여유
	// 손 붙이기 — Lyra 는 IK 총 뼈를 오른손 자리에 두고, 왼손을 그 아래 IK 왼손 뼈로 당긴다(왼손이 총 손잡이에 붙는다).
	// 확인: 공중에서 IK 총 뼈 = 오른손(거리 0.0) · 오른손은 이 보정이 없던 단순 계산과 같다(서 있는 구간 차이 = 발 디딤 4.0 뿐) · 서 있는 동안 두 손 사이 36.2cm 고정.
	// 단순 계산은 아래를 볼 때(-60도) 두 손 사이가 53cm 까지 벌어졌다. 오른손까지 IK 뼈로 당기면 Lyra 와 멀어졌다(아래 60도 유지 구간 오른손 4.0 → 7.2cm)
	const TCHAR* const HAND_R = TEXT("hand_r");
	const TCHAR* const IK_GUN = TEXT("ik_hand_gun");
	const TCHAR* const IK_HAND_L = TEXT("ik_hand_l");
	const TCHAR* const LEFT_ARM[] = { TEXT("upperarm_l"), TEXT("lowerarm_l"), TEXT("hand_l") };
	constexpr float GROUND_TRACE_DISTANCE = 100000.f;	// 땅까지 거리를 재는 선 길이 — Lyra GetGroundInfo 와 같은 값
	constexpr int32 LAND_TABLE_HZ = 120;		// 착지 클립 거리 곡선을 뽑는 간격

	float Smooth(float X)
	{
		X = FMath::Clamp(X, 0.f, 1.f);
		return X * X * (3.f - 2.f * X);
	}

	// 땅까지 거리 — Lyra ULyraCharacterMovementComponent::GetGroundInfo 와 같은 방식(몸 가운데에서 아래로 선을 쏘아 맞은 거리 - 캡슐 반 높이)
	float MeasureGroundDistance(const ACharacter* Character, const UCharacterMovementComponent* Move)
	{
		const UCapsuleComponent* Capsule = Character ? Character->GetCapsuleComponent() : nullptr;
		const UWorld* World = Character ? Character->GetWorld() : nullptr;
		if (!Capsule || !Move || !World) return GROUND_TRACE_DISTANCE;
		const float HalfHeight = Capsule->GetUnscaledCapsuleHalfHeight();
		const FVector Start = Character->GetActorLocation();
		const FVector End(Start.X, Start.Y, Start.Z - GROUND_TRACE_DISTANCE - HalfHeight);
		FCollisionQueryParams Params(SCENE_QUERY_STAT(MOBSimpleGroundDistance), false, Character);
		FCollisionResponseParams Response;
		Move->InitCollisionParams(Params, Response);
		const ECollisionChannel Channel = Move->UpdatedComponent ? Move->UpdatedComponent->GetCollisionObjectType() : ECC_Pawn;
		FHitResult Hit;
		World->LineTraceSingleByChannel(Hit, Start, End, Channel, Params, Response);
		return Hit.bBlockingHit ? FMath::Max((float)Hit.Distance - HalfHeight, 0.f) : GROUND_TRACE_DISTANCE;
	}

	// 메시 뿌리가 바닥보다 얼마나 위에 있나(cm). 바닥 = 캡슐 바닥 - 이동 컴포넌트가 잰 바닥까지 거리. 바닥을 못 찾았으면 false
	bool MeasurePlantGap(const ACharacter* Character, const UCharacterMovementComponent* Move, float& OutGap, float& OutFloorDistance)
	{
		const UCapsuleComponent* Capsule = Character ? Character->GetCapsuleComponent() : nullptr;
		const USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
		if (!Capsule || !Mesh || !Move || !Move->CurrentFloor.bBlockingHit) return false;
		OutFloorDistance = Move->CurrentFloor.GetDistanceToFloor();
		const double FloorZ = Character->GetActorLocation().Z - Capsule->GetScaledCapsuleHalfHeight() - OutFloorDistance;
		OutGap = (float)(Mesh->GetComponentLocation().Z - FloorZ);
		return true;
	}

	FCompactPoseBoneIndex FindCompactBone(const FBoneContainer& Bones, const TCHAR* Name)
	{
		FBoneReference Ref(Name);
		Ref.Initialize(Bones);
		return Ref.IsValidToEvaluate(Bones) ? Ref.GetCompactPoseIndex(Bones) : FCompactPoseBoneIndex(INDEX_NONE);
	}
}

FAnimInstanceProxy* UMOBSimpleAnimInstance::CreateAnimInstanceProxy()
{
	return new FMOBSimpleAnimProxy(this);
}

void UMOBSimpleAnimInstance::LoadClips()
{
	if (bClipsLoaded) return;
	bClipsLoaded = true;
	bAimJump = FParse::Param(FCommandLine::Get(), TEXT("MOBSimpleAimJump"));
	const TCHAR* const RIFLE_FOLDER = TEXT("/Game/Characters/Heroes/Mannequin/Animations/Locomotion/Rifle");
	const TCHAR* const AIM_FOLDER = TEXT("/Game/Characters/Heroes/Mannequin/Animations/AimOffsets");
	const auto Load = [](const TCHAR* Folder, const TCHAR* Name, const TCHAR* Kind)
	{
		UAnimSequence* Clip = LoadObject<UAnimSequence>(nullptr, *FString::Printf(TEXT("%s/%s.%s"), Folder, Name, Name));
		// 곡선 이름도 남긴다 — 착지 클립을 「땅까지 거리」 곡선으로 맞추려면 그 곡선이 있어야 한다
		FString Curves;
		if (Clip)
		{
			for (const FFloatCurve& Curve : Clip->GetCurveData().FloatCurves) Curves += Curve.GetName().ToString() + TEXT(" ");
		}
		UE_LOG(LogMOBSimple, Log, TEXT("MOB_SIMPLE_%s name=%s ok=%d additive=%d length=%.3f curves=[%s]"), Kind, Name, Clip ? 1 : 0, Clip ? (int32)Clip->GetAdditiveAnimType() : -1, Clip ? Clip->GetPlayLength() : 0.f, *Curves);
		if (!Clip) UE_LOG(LogMOBSimple, Error, TEXT("MOB_SIMPLE_FAIL clip=%s"), Name);
		return Clip;
	};

	for (int32 I = 0; I < FMOBSimpleAnimProxy::kClips; ++I) LoadedClips.Add(Load(RIFLE_FOLDER, ClipName(bAimJump, I), TEXT("LOAD")));
	if (!bAimJump) return;
	for (int32 I = 0; I < FMOBSimpleAnimProxy::kAim; ++I) LoadedAim.Add(Load(AIM_FOLDER, AIM_NAMES[I], TEXT("AIM")));
	for (int32 I = 0; I < FMOBSimpleAnimProxy::kAim; ++I) LoadedReadyAim.Add(Load(AIM_FOLDER, READY_AIM_NAMES[I], TEXT("AIM")));
	for (int32 I = 0; I < FMOBSimpleAnimProxy::kJump; ++I) LoadedJump.Add(Load(RIFLE_FOLDER, JUMP_NAMES[I], TEXT("JUMP")));
	LoadedRecovery = Load(RIFLE_FOLDER, RECOVERY_NAME, TEXT("JUMP"));
	LoadedFidget = Load(RIFLE_FOLDER, FIDGET_NAME, TEXT("IDLE"));

	// 세 자세가 맡는 위아래 각은 짐작하지 않고 Lyra 조준 보정 자산의 표본 자리에서 읽는다
	const auto ReadPitches = [](const TCHAR* AssetPath, const TArray<TObjectPtr<UAnimSequence>>& Set, const TCHAR* const Names[], float OutPitches[])
	{
		const UBlendSpace* AO = LoadObject<UBlendSpace>(nullptr, AssetPath);
		int32 Matched = 0;
		if (AO)
		{
			for (int32 Axis = 0; Axis < 2; ++Axis)
			{
				const FBlendParameter& P = AO->GetBlendParameter(Axis);
				UE_LOG(LogMOBSimple, Log, TEXT("MOB_SIMPLE_AO asset=%s axis=%d name=%s min=%.1f max=%.1f"), *GetNameSafe(AO), Axis, *P.DisplayName, P.Min, P.Max);
			}
			for (const FBlendSample& S : AO->GetBlendSamples())
			{
				for (int32 I = 0; I < FMOBSimpleAnimProxy::kAim; ++I)
				{
					if (Set.IsValidIndex(I) && Set[I] && S.Animation == Set[I])
					{
						OutPitches[I] = (float)S.SampleValue.Y;
						++Matched;
						UE_LOG(LogMOBSimple, Log, TEXT("MOB_SIMPLE_AO sample=%s x=%.1f y=%.1f"), Names[I], S.SampleValue.X, S.SampleValue.Y);
					}
				}
			}
		}
		if (Matched != FMOBSimpleAnimProxy::kAim)
		{
			UE_LOG(LogMOBSimple, Error, TEXT("MOB_SIMPLE_FAIL aimoffset asset=%s found=%d matched=%d — 위 90 · 가운데 0 · 아래 -90 으로 둔다"), AssetPath, AO ? 1 : 0, Matched);
		}
	};
	ReadPitches(AIM_OFFSET_PATH, LoadedAim, AIM_NAMES, AimPitches);
	ReadPitches(READY_AIM_OFFSET_PATH, LoadedReadyAim, READY_AIM_NAMES, ReadyAimPitches);
}

void UMOBSimpleAnimInstance::ResetPlayback()
{
	GetProxyOnGameThread<FMOBSimpleAnimProxy>().ResetPlayback();
}

void FMOBSimpleAnimProxy::ResetPlayback()
{
	for (int32 I = 0; I < kClips; ++I) Weights[I] = I == CLIP_IDLE ? 1.f : 0.f;
	Step = 0;
	StepFrac = 0.f;
	// 판 시작(순간이동 + 멈춤)도 「방금 멈췄다」로 본다 — 대기 몸짓 시계(StandTime)와 같은 기준
	IdleTime = bAimJump && Lengths[CLIP_IDLE] > STOP_CLIP_SECONDS ? Lengths[CLIP_IDLE] - STOP_CLIP_SECONDS : 0.f;
	bWasAccelerating = false;
	GroundTime = PLANT_LAND_DELAY;
	AirTime = PLANT_RELEASE_SECONDS;
	PlantDrop = 0.f;
	AimBlend = 1.f;
	JumpCount = 0;
	JumpClip = -1;
	JumpWeight = 0.f;
	bWasFalling = false;
	RecoveryTime = -1.f;
	StandTime = 0.f;
	FidgetTime = -1.f;
	bFidgetLeaving = false;
}

void FMOBSimpleAnimProxy::GetAimJump(float& OutPitch, int32& OutJumpClip, float& OutJumpWeight) const
{
	OutPitch = Pitch;
	OutJumpClip = JumpClip;
	OutJumpWeight = JumpWeight;
}

// 대기 몸짓 — 멈추고 STOP_CLIP_SECONDS + IDLE_BREAK_DELAY 초 가만히 서 있으면 한 번 보인다. 움직이기 시작하면 서기 몫과 함께 빠지게 두었다가 다 빠지면 끝낸다
// (Lyra 실행값도 달리기 시작 뒤 서기 클립으로 돌아가지 않고 몸짓째 빠졌다)
void FMOBSimpleAnimProxy::UpdateIdleBreak(bool bStill, float DeltaSeconds)
{
	if (FidgetTime >= 0.f)
	{
		FidgetTime += DeltaSeconds;
		if (!bStill) bFidgetLeaving = true;
		if (FidgetTime >= FidgetLength || (bFidgetLeaving && Weights[CLIP_IDLE] <= ZERO_ANIMWEIGHT_THRESH))
		{
			FidgetTime = -1.f;
			bFidgetLeaving = false;
			StandTime = 0.f;
		}
		return;
	}
	StandTime = bStill ? StandTime + DeltaSeconds : 0.f;
	if (FidgetClip && StandTime >= STOP_CLIP_SECONDS + IDLE_BREAK_DELAY)
	{
		FidgetTime = 0.f;
		StandTime = 0.f;
	}
}

// 서기 자리에서 대기 몸짓이 차지하는 몫 — 들어올 때와 클립 끝에서 부드럽게 넣고 뺀다
float FMOBSimpleAnimProxy::FidgetAlpha() const
{
	if (FidgetTime < 0.f || !FidgetClip) return 0.f;
	return Smooth(FMath::Min(FidgetTime, FidgetLength - FidgetTime) / IDLE_BREAK_BLEND);
}

// 점프 쪽 상태를 하나 더 얹는다. 쌓기가 비었으면 걷기·서기부터 깐다. 자리가 차면 가장 아래 것을 버린다(그때쯤이면 몫이 거의 없다)
void FMOBSimpleAnimProxy::PushJump(int32 Clip, float FadeSeconds)
{
	if (JumpCount == 0)
	{
		JumpStack[0] = FJumpLayer();
		JumpCount = 1;
	}
	if (JumpCount == kJumpStack)
	{
		for (int32 I = 1; I < kJumpStack; ++I) JumpStack[I - 1] = JumpStack[I];
		--JumpCount;
	}
	FJumpLayer& Layer = JumpStack[JumpCount++];
	Layer.Clip = Clip;
	Layer.Time = 0.f;
	Layer.Fade = FadeSeconds > 0.f ? 0.f : 1.f;
	Layer.FadeSeconds = FadeSeconds;
}

// 쌓인 상태마다의 몫 — 위 것이 덮은 만큼 아래 것들이 함께 줄어든다
void FMOBSimpleAnimProxy::JumpWeights(float Out[kJumpStack]) const
{
	float Remain = 1.f;
	for (int32 I = JumpCount - 1; I >= 0; --I)
	{
		const FJumpLayer& Layer = JumpStack[I];
		const float Alpha = I == 0 ? 1.f : Smooth(Layer.Fade);	// 실행값의 상태 전환은 모두 부드러운 곡선이었다
		Out[I] = Remain * Alpha;
		Remain -= Out[I];
	}
}

// 착지 클립에서 땅까지 거리가 이 값이 되는 시각. 곡선은 시간이 갈수록 줄어 클립 끝에서 0 이다
float FMOBSimpleAnimProxy::LandTimeFromDistance(float GroundDistance) const
{
	const int32 Num = LandDistances.Num();
	if (Num == 0 || GroundDistance >= LandDistances[0]) return 0.f;
	for (int32 K = 1; K < Num; ++K)
	{
		if (LandDistances[K] <= GroundDistance)
		{
			const float Span = LandDistances[K - 1] - LandDistances[K];
			const float Frac = Span > UE_SMALL_NUMBER ? (LandDistances[K - 1] - GroundDistance) / Span : 1.f;
			return FMath::Min(((float)(K - 1) + Frac) / LAND_TABLE_HZ, JumpLengths[JUMP_LAND]);
		}
	}
	return JumpLengths[JUMP_LAND];
}

// 점프 상태 — Lyra 실행값의 차례와 때를 따른다. 넘어갈지는 이번 프레임 입력과 지난 프레임까지 흐른 클립 시각으로 정하고, 정한 뒤 시간을 흘린다
// (새 상태도 들어온 프레임에 한 프레임만큼 간다 — 실행값이 그랬다)
//   뛰어오름 →(클립 끝) 오르는 중 →(꼭대기까지 APEX_LEAD_SECONDS 미만) 꼭대기 →(클립이 APEX_EXIT_REMAINING 못 남음) 떨어지는 중 →(땅까지 거리가 착지 클립 곡선 안) 착지 클립
//   착지 클립은 땅까지 거리로 시각을 맞춰 땅에 닿을 때 끝난다. 걸어서 떨어지면(뜰 때 위 속도 ≤ 0) 바로 떨어지는 중.
//   땅에 닿으면 걷기·서기를 얹고 착지 충격 덧붙임을 시작한다
void FMOBSimpleAnimProxy::UpdateJump(bool bFalling, bool bAccelerating, float VelZ, float TimeToApex, float GroundDistance, float DeltaSeconds)
{
	const int32 TopClip = JumpCount > 0 ? JumpStack[JumpCount - 1].Clip : JUMP_NONE;
	const float TopTime = JumpCount > 0 ? JumpStack[JumpCount - 1].Time : 0.f;
	if (bFalling && !bWasFalling)
	{
		PushJump(VelZ > 0.f ? JUMP_START : JUMP_FALL_LOOP, JUMP_BLEND_SECONDS);
	}
	else if (bFalling)
	{
		switch (TopClip)
		{
		case JUMP_START:
			if (TopTime >= JumpLengths[JUMP_START] - KINDA_SMALL_NUMBER) PushJump(JUMP_START_LOOP, 0.f);	// 실행값: 섞지 않고 바로 넘어감
			break;
		case JUMP_START_LOOP:
			if (TimeToApex < APEX_LEAD_SECONDS) PushJump(JUMP_APEX, JUMP_BLEND_SECONDS);
			break;
		case JUMP_APEX:
			if (JumpLengths[JUMP_APEX] - TopTime < APEX_EXIT_REMAINING) PushJump(JUMP_FALL_LOOP, JUMP_BLEND_SECONDS);
			break;
		case JUMP_FALL_LOOP:
			if (LandDistances.Num() > 0 && GroundDistance < LandDistances[0]) PushJump(JUMP_LAND, LAND_FADE_SECONDS);
			break;
		default:
			break;
		}
	}
	else if (bWasFalling)
	{
		PushJump(JUMP_NONE, bAccelerating ? LANDED_MOVE_SECONDS : LANDED_STAND_SECONDS);
		RecoveryTime = 0.f;
		IdleTime = 0.f;		// Lyra 도 내려앉으면 서기 클립을 처음부터 튼다
	}
	bWasFalling = bFalling;

	for (int32 I = 0; I < JumpCount; ++I)
	{
		FJumpLayer& Layer = JumpStack[I];
		Layer.Time = Layer.Clip == JUMP_LAND && LandDistances.Num() > 0 ? FMath::Max(Layer.Time, LandTimeFromDistance(GroundDistance)) : Layer.Time + DeltaSeconds;
		if (Layer.FadeSeconds > 0.f) Layer.Fade = FMath::Min(Layer.Fade + DeltaSeconds / Layer.FadeSeconds, 1.f);
	}
	// 다 덮인 아래 상태는 버린다. 걷기·서기만 남으면 점프 쪽을 끈다
	for (int32 I = JumpCount - 1; I > 0; --I)
	{
		if (JumpStack[I].Fade < 1.f) continue;
		for (int32 J = I; J < JumpCount; ++J) JumpStack[J - I] = JumpStack[J];
		JumpCount -= I;
		break;
	}
	if (JumpCount == 1 && JumpStack[0].Clip == JUMP_NONE) JumpCount = 0;

	float Mix[kJumpStack] = {};
	JumpWeights(Mix);
	JumpWeight = 0.f;
	for (int32 I = 0; I < JumpCount; ++I)
	{
		if (JumpStack[I].Clip != JUMP_NONE) JumpWeight += Mix[I];
	}
	JumpClip = JumpCount > 0 ? JumpStack[JumpCount - 1].Clip : -1;

	if (RecoveryTime >= 0.f)
	{
		RecoveryTime += DeltaSeconds;
		if (!RecoveryClip || RecoveryTime >= RecoveryLength) RecoveryTime = -1.f;
	}
}

// 쌓인 점프 상태를 몫대로 섞는다. 걷기·서기 자리는 이미 계산해 둔 Output 자세다
void FMOBSimpleAnimProxy::ApplyJump(FPoseContext& Output) const
{
	if (JumpCount == 0) return;
	float Layers[kJumpStack] = {};
	JumpWeights(Layers);

	TArray<FCompactPose, TInlineAllocator<kJumpStack + 1>> Poses;
	TArray<FBlendedCurve, TInlineAllocator<kJumpStack + 1>> Curves;
	TArray<UE::Anim::FStackAttributeContainer, TInlineAllocator<kJumpStack + 1>> Attributes;
	TArray<float, TInlineAllocator<kJumpStack + 1>> Mix;
	float BaseWeight = 0.f;
	for (int32 I = 0; I < JumpCount; ++I)
	{
		if (Layers[I] <= ZERO_ANIMWEIGHT_THRESH) continue;
		const FJumpLayer& Layer = JumpStack[I];
		if (Layer.Clip == JUMP_NONE || !JumpClips[Layer.Clip])
		{
			BaseWeight += Layers[I];
			continue;
		}
		const bool bLoop = Layer.Clip == JUMP_START_LOOP || Layer.Clip == JUMP_FALL_LOOP;
		const float Length = FMath::Max(JumpLengths[Layer.Clip], 0.01f);
		const float Time = bLoop ? FMath::Fmod(Layer.Time, Length) : FMath::Min(Layer.Time, Length);
		FPoseContext Jump(Output);
		FAnimationPoseData JumpData(Jump);
		JumpClips[Layer.Clip]->GetAnimationPose(JumpData, FAnimExtractContext((double)Time, false, FDeltaTimeRecord(), bLoop));
		Poses.AddDefaulted_GetRef().MoveBonesFrom(Jump.Pose);
		Curves.AddDefaulted_GetRef().MoveFrom(Jump.Curve);
		Attributes.AddDefaulted_GetRef().MoveFrom(Jump.CustomAttributes);
		Mix.Add(Layers[I]);
	}
	if (Poses.Num() == 0) return;
	if (BaseWeight > ZERO_ANIMWEIGHT_THRESH)
	{
		Poses.AddDefaulted_GetRef().CopyBonesFrom(Output.Pose);
		Curves.AddDefaulted_GetRef().CopyFrom(Output.Curve);
		Attributes.AddDefaulted_GetRef().CopyFrom(Output.CustomAttributes);
		Mix.Add(BaseWeight);
	}
	float Sum = 0.f;
	for (const float W : Mix) Sum += W;
	for (float& W : Mix) W /= Sum;
	FAnimationPoseData OutputData(Output);
	FAnimationRuntime::BlendPosesTogether(Poses, Curves, Attributes, Mix, OutputData);
}

// 내려앉은 뒤 착지 충격 덧붙임을 얹는다(Lyra 는 온몸 덧붙임 상태 기계가 이 일을 한다)
void FMOBSimpleAnimProxy::ApplyRecovery(FPoseContext& Output) const
{
	if (RecoveryTime < 0.f || !RecoveryClip || RecoveryAdditiveType == AAT_None) return;
	const float Weight = RECOVERY_WEIGHT * Smooth(FMath::Min(RecoveryTime, RecoveryLength - RecoveryTime) / RECOVERY_BLEND_SECONDS);
	if (Weight <= ZERO_ANIMWEIGHT_THRESH) return;
	FPoseContext Additive(Output, true);
	FAnimationPoseData AdditiveData(Additive);
	RecoveryClip->GetAnimationPose(AdditiveData, FAnimExtractContext((double)FMath::Min(RecoveryTime, RecoveryLength), false, FDeltaTimeRecord(), false));
	FAnimationPoseData OutputData(Output);
	FAnimationRuntime::AccumulateAdditivePose(OutputData, AdditiveData, Weight, (EAdditiveAnimationType)RecoveryAdditiveType);
	Output.Pose.NormalizeRotations();
}

// 위아래 조준 한 벌 — 이웃한 두 자세(가운데와 위, 또는 가운데와 아래)를 각도로 섞어 Weight 만큼 덧붙인다. Lyra 조준 보정 노드가 하는 일을 가운데 줄만으로 한 것
void FMOBSimpleAnimProxy::AccumulateAim(FPoseContext& Output, const UAnimSequence* const Set[kAim], const float SetPitches[kAim], int32 AdditiveType, float Weight) const
{
	if (Weight <= ZERO_ANIMWEIGHT_THRESH || !Set[AIM_UP] || !Set[AIM_CENTER] || !Set[AIM_DOWN] || AdditiveType == AAT_None) return;
	const int32 Other = Pitch >= SetPitches[AIM_CENTER] ? AIM_UP : AIM_DOWN;
	const float Span = SetPitches[Other] - SetPitches[AIM_CENTER];
	const float Alpha = FMath::Abs(Span) > UE_SMALL_NUMBER ? FMath::Clamp((Pitch - SetPitches[AIM_CENTER]) / Span, 0.f, 1.f) : 0.f;

	FPoseContext Center(Output, true);
	FAnimationPoseData CenterData(Center);
	Set[AIM_CENTER]->GetAnimationPose(CenterData, FAnimExtractContext(0.0, false, FDeltaTimeRecord(), false));
	FPoseContext Edge(Output, true);
	FAnimationPoseData EdgeData(Edge);
	Set[Other]->GetAnimationPose(EdgeData, FAnimExtractContext(0.0, false, FDeltaTimeRecord(), false));
	FPoseContext Additive(Output, true);
	FAnimationPoseData AdditiveData(Additive);
	FAnimationRuntime::BlendTwoPosesTogether(CenterData, EdgeData, 1.f - Alpha, AdditiveData);

	FAnimationPoseData OutputData(Output);
	FAnimationRuntime::AccumulateAdditivePose(OutputData, AdditiveData, Weight, (EAdditiveAnimationType)AdditiveType);
	Output.Pose.NormalizeRotations();
}

// 서 있을 땐 허리 사격 조준, 이동 입력이 있으면 느슨한 조준 — 둘 사이는 AimBlend 로 옮겨 간다.
// Lyra 는 두 조준을 각각 얹은 자세끼리 섞는데 여기선 두 덧붙임을 차례로 몫만큼 얹는다. 옮겨 가는 0.3초 동안만 조금 다르고 양 끝(0 · 1)은 같다
void FMOBSimpleAnimProxy::ApplyAim(FPoseContext& Output) const
{
	AccumulateAim(Output, AimClips, AimPitches, AimAdditiveType, AimBlend);
	AccumulateAim(Output, ReadyAimClips, ReadyAimPitches, ReadyAimAdditiveType, 1.f - AimBlend);
}

void FMOBSimpleAnimProxy::GetState(float OutWeights[kClips], float OutTimes[kClips]) const
{
	for (int32 I = 0; I < kClips; ++I)
	{
		OutWeights[I] = Weights[I];
		OutTimes[I] = Times[I];
	}
}

void FMOBSimpleAnimProxy::AppendClipDump(FString& Out, int32 SampleHz, const TSet<int32>& KeepBones) const
{
	// 자세를 읽는 길은 Evaluate 와 같다(GetAnimationPose, 뿌리 이동 안 뽑음, 반복 재생)
	const FBoneContainer& Bones = GetRequiredBones();
	FMemMark Mark(FMemStack::Get());
	const auto Line = [&Out](const TCHAR* Head, const FTransform& T)
	{
		const FVector P = T.GetTranslation();
		const FQuat Q = T.GetRotation();
		const FVector S = T.GetScale3D();
		Out += FString::Printf(TEXT("%s,%.6f,%.6f,%.6f,%.9f,%.9f,%.9f,%.9f,%.6f,%.6f,%.6f\n"), Head, P.X, P.Y, P.Z, Q.X, Q.Y, Q.Z, Q.W, S.X, S.Y, S.Z);
	};

	const int32 NumBones = Bones.GetCompactPoseNumBones();
	for (int32 B = 0; B < NumBones; ++B)
	{
		const FCompactPoseBoneIndex Index(B);
		const int32 Parent = Index.IsRootBone() ? -1 : Bones.GetParentBoneIndex(Index).GetInt();
		const FName Name = Bones.GetReferenceSkeleton().GetBoneName(Bones.MakeMeshPoseIndex(Index).GetInt());
		Line(*FString::Printf(TEXT("BONE,%d,%s,%d"), B, *Name.ToString(), Parent), Bones.GetRefPoseTransform(Index));
	}

	for (int32 I = 0; I < kClips; ++I)
	{
		if (!Clips[I]) continue;
		const int32 Samples = FMath::CeilToInt32(Lengths[I] * SampleHz) + 1;
		Out += FString::Printf(TEXT("CLIP,%d,%s,%.9g,%d\n"), I, ClipName(bAimJump, I), Lengths[I], Samples);
		for (int32 S = 0; S < Samples; ++S)
		{
			const double Time = FMath::Min((double)S / SampleHz, (double)Lengths[I]);
			FCompactPose Pose;
			Pose.SetBoneContainer(&Bones);
			FBlendedCurve Curve;
			Curve.InitFrom(Bones);
			UE::Anim::FStackAttributeContainer Attributes;
			FAnimationPoseData Data(Pose, Curve, Attributes);
			Clips[I]->GetAnimationPose(Data, FAnimExtractContext(Time, false, FDeltaTimeRecord(), true));
			for (int32 B = 0; B < NumBones; ++B)
			{
				if (!KeepBones.Contains(B)) continue;
				Line(*FString::Printf(TEXT("KEY,%d,%d,%d"), I, S, B), Pose[FCompactPoseBoneIndex(B)]);
			}
		}
	}
}

void FMOBSimpleAnimProxy::PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds)
{
	FAnimInstanceProxy::PreUpdate(InAnimInstance, DeltaSeconds);

	UMOBSimpleAnimInstance* Instance = CastChecked<UMOBSimpleAnimInstance>(InAnimInstance);
	if (!Instance->bClipsLoaded)
	{
		Instance->LoadClips();
		bAimJump = Instance->bAimJump;

		for (int32 I = 0; I < kClips; ++I)
		{
			const UAnimSequence* Clip = Instance->LoadedClips.IsValidIndex(I) ? Instance->LoadedClips[I].Get() : nullptr;
			Clips[I] = Clip;
			if (!Clip) continue;
			Lengths[I] = FMath::Max(Clip->GetPlayLength(), 0.01f);
			RootSpeeds[I] = (float)Clip->ExtractRootMotionFromRange(0.0, Lengths[I], FAnimExtractContext()).GetTranslation().Size() / Lengths[I];

			// 발 표시를 왼발부터 줄 세운다. 왼발·오른발이 번갈아 있고 개수가 짝수여야 한 바퀴 돌아도 발 순서가 안 깨진다
			const TArray<FAnimSyncMarker>& Markers = Clip->AuthoredSyncMarkers;
			int32 FirstLeft = 0;
			while (FirstLeft < Markers.Num() && Markers[FirstLeft].MarkerName != TEXT("L")) ++FirstLeft;
			FString MarkerList;
			StepTimes[I].Reset();
			for (int32 M = 0; M < Markers.Num() && FirstLeft < Markers.Num(); ++M)
			{
				const int32 Index = (FirstLeft + M) % Markers.Num();
				StepTimes[I].Add(Markers[Index].Time + (Index < FirstLeft ? Lengths[I] : 0.f));
				MarkerList += FString::Printf(TEXT("%s@%.3f "), *Markers[Index].MarkerName.ToString(), Markers[Index].Time);
			}
			UE_LOG(LogMOBSimple, Log, TEXT("MOB_SIMPLE_CLIP name=%s length=%.3f rootSpeed=%.1f steps=%d markers=[%s]"), ClipName(bAimJump, I), Lengths[I], RootSpeeds[I], StepTimes[I].Num(), *MarkerList);
			if (I != CLIP_IDLE && (StepTimes[I].Num() == 0 || StepTimes[I].Num() % 2 != 0))
			{
				UE_LOG(LogMOBSimple, Error, TEXT("MOB_SIMPLE_FAIL steps name=%s count=%d"), ClipName(bAimJump, I), StepTimes[I].Num());
				Clips[I] = nullptr;		// 발을 못 맞추는 클립은 쓰지 않는다
			}
		}
		StepCycle = 1;
		for (int32 I = CLIP_FWD; I <= CLIP_RIGHT; ++I)
		{
			if (Clips[I]) StepCycle = FMath::LeastCommonMultiplier(StepCycle, StepTimes[I].Num());
		}

		const auto Additive = [](const UAnimSequence* Clip) { return Clip ? (int32)Clip->GetAdditiveAnimType() : (int32)AAT_None; };
		for (int32 I = 0; I < kAim && bAimJump; ++I)
		{
			AimClips[I] = Instance->LoadedAim.IsValidIndex(I) ? Instance->LoadedAim[I].Get() : nullptr;
			AimPitches[I] = Instance->AimPitches[I];
			ReadyAimClips[I] = Instance->LoadedReadyAim.IsValidIndex(I) ? Instance->LoadedReadyAim[I].Get() : nullptr;
			ReadyAimPitches[I] = Instance->ReadyAimPitches[I];
		}
		AimAdditiveType = Additive(AimClips[AIM_CENTER]);
		ReadyAimAdditiveType = Additive(ReadyAimClips[AIM_CENTER]);
		for (int32 I = 0; I < kJump && bAimJump; ++I)
		{
			JumpClips[I] = Instance->LoadedJump.IsValidIndex(I) ? Instance->LoadedJump[I].Get() : nullptr;
			JumpLengths[I] = JumpClips[I] ? FMath::Max(JumpClips[I]->GetPlayLength(), 0.01f) : 0.f;
		}
		RecoveryClip = bAimJump ? Instance->LoadedRecovery.Get() : nullptr;
		RecoveryLength = RecoveryClip ? FMath::Max(RecoveryClip->GetPlayLength(), 0.01f) : 0.f;
		RecoveryAdditiveType = Additive(RecoveryClip);
		FidgetClip = bAimJump ? Instance->LoadedFidget.Get() : nullptr;
		FidgetLength = FidgetClip ? FMath::Max(FidgetClip->GetPlayLength(), 0.01f) : 0.f;

		// 착지 클립의 땅까지 거리 곡선을 표로 뽑아 둔다 — 공중에서 지금 거리에 맞는 시각을 찾는 데 쓴다
		LandDistances.Reset();
		const UAnimSequence* Land = JumpClips[JUMP_LAND];
		bool bLandCurve = false;
		if (Land)
		{
			for (const FFloatCurve& Curve : Land->GetCurveData().FloatCurves) bLandCurve |= Curve.GetName() == LAND_DISTANCE_CURVE;
		}
		if (bLandCurve)
		{
			const int32 Samples = FMath::CeilToInt32(JumpLengths[JUMP_LAND] * LAND_TABLE_HZ) + 1;
			for (int32 S = 0; S < Samples; ++S)
			{
				const double Time = FMath::Min((double)S / LAND_TABLE_HZ, (double)JumpLengths[JUMP_LAND]);
				LandDistances.Add(-Land->EvaluateCurveData(LAND_DISTANCE_CURVE, FAnimExtractContext(Time)));	// 곡선은 땅 위를 음수로 담는다(시작 -203 → 끝 0)
			}
		}
		if (bAimJump)
		{
			int32 Rises = 0;	// 시간이 가며 거리가 다시 커지는 칸 수 — 0 이어야 거리로 시각을 하나로 찾는다
			for (int32 K = 1; K < LandDistances.Num(); ++K) Rises += LandDistances[K] > LandDistances[K - 1] + 0.01f ? 1 : 0;
			const auto LandAt = [this](float Time) { return LandDistances.Num() > 0 ? LandDistances[FMath::Clamp(FMath::RoundToInt32(Time * LAND_TABLE_HZ), 0, LandDistances.Num() - 1)] : -1.f; };
			UE_LOG(LogMOBSimple, Log, TEXT("MOB_SIMPLE_AIMJUMP pitches=%.1f/%.1f/%.1f ready=%.1f/%.1f/%.1f additive=%d/%d stepCycle=%d"),
				AimPitches[AIM_UP], AimPitches[AIM_CENTER], AimPitches[AIM_DOWN], ReadyAimPitches[AIM_UP], ReadyAimPitches[AIM_CENTER], ReadyAimPitches[AIM_DOWN], AimAdditiveType, ReadyAimAdditiveType, StepCycle);
			UE_LOG(LogMOBSimple, Log, TEXT("MOB_SIMPLE_LAND curve=%d samples=%d rises=%d at0=%.1f at0.264=%.1f at0.335=%.1f end=%.1f recovery=%.3f/%d fidget=%.3f"),
				bLandCurve ? 1 : 0, LandDistances.Num(), Rises, LandAt(0.f), LandAt(0.264f), LandAt(0.335f), LandAt(JumpLengths[JUMP_LAND]), RecoveryLength, RecoveryAdditiveType, FidgetLength);
			if (!bLandCurve) UE_LOG(LogMOBSimple, Error, TEXT("MOB_SIMPLE_FAIL land curve=%s — 착지 클립을 거리로 못 맞춰 떨어지는 중에서 바로 땅에 닿는다"), *LAND_DISTANCE_CURVE.ToString());
		}
	}

	// 입력은 속도와 몸 방향뿐이다
	const AActor* Owner = InAnimInstance->GetOwningActor();
	const FVector Velocity = Owner ? Owner->GetVelocity() : FVector::ZeroVector;
	const FVector Local = Owner ? Owner->GetActorRotation().UnrotateVector(Velocity) : FVector::ZeroVector;
	const float Speed = (float)Local.Size2D();

	float Targets[kClips] = {};
	const float MoveAlpha = FMath::Clamp(Speed / MOVE_FULL_SPEED, 0.f, 1.f);
	Targets[CLIP_IDLE] = 1.f - MoveAlpha;
	if (Speed > 1.f)
	{
		// 0 = 앞, +90 = 오른쪽, ±180 = 뒤, -90 = 왼쪽 — 이웃한 두 방향 클립을 각도로 나눈다
		const float Angle = FMath::RadiansToDegrees(FMath::Atan2((float)Local.Y, (float)Local.X));
		const float Abs = FMath::Abs(Angle);
		const EClip Side = Angle >= 0.f ? CLIP_RIGHT : CLIP_LEFT;
		if (Abs <= 90.f)
		{
			Targets[CLIP_FWD] = (1.f - Abs / 90.f) * MoveAlpha;
			Targets[Side] = (Abs / 90.f) * MoveAlpha;
		}
		else
		{
			Targets[Side] = (1.f - (Abs - 90.f) / 90.f) * MoveAlpha;
			Targets[CLIP_BWD] = ((Abs - 90.f) / 90.f) * MoveAlpha;
		}
	}

	const float Follow = 1.f - FMath::Exp(-DeltaSeconds / BLEND_SECONDS);
	float Sum = 0.f;
	for (int32 I = 0; I < kClips; ++I)
	{
		Weights[I] += (Targets[I] - Weights[I]) * Follow;
		Sum += Weights[I];
	}
	if (Sum > UE_SMALL_NUMBER)
	{
		for (float& Weight : Weights) Weight /= Sum;
	}

	// 달리기 진행 — 클립의 원래 속도와 실제 속도의 비로 재생해 발이 바닥에서 덜 미끄러지게 하고,
	// 발걸음 단위로 함께 넘어가 섞이는 두 클립의 발이 같은 때 닿게 한다(언리얼 동기화 그룹이 발 표시로 하는 일을 직접 한 것)
	const auto StepSpan = [this](int32 I, float& OutStart, float& OutEnd)
	{
		const TArray<float, TInlineAllocator<8>>& Table = StepTimes[I];
		const int32 K = Step % Table.Num();
		OutStart = Table[K];
		OutEnd = K + 1 < Table.Num() ? Table[K + 1] : Table[0] + Lengths[I];
	};

	float DirectionWeight = 0.f, RootSpeed = 0.f, StepSeconds = 0.f;
	for (int32 I = CLIP_FWD; I <= CLIP_RIGHT; ++I)
	{
		if (!Clips[I]) continue;
		float Start, End;
		StepSpan(I, Start, End);
		DirectionWeight += Weights[I];
		RootSpeed += Weights[I] * RootSpeeds[I];
		StepSeconds += Weights[I] * (End - Start);
	}
	if (DirectionWeight > 0.001f && StepSeconds > UE_SMALL_NUMBER)
	{
		RootSpeed /= DirectionWeight;
		StepSeconds /= DirectionWeight;
		const float PlayRate = RootSpeed > 1.f ? FMath::Clamp(Speed / RootSpeed, MIN_PLAY_RATE, MAX_PLAY_RATE) : 1.f;
		StepFrac += DeltaSeconds * PlayRate / StepSeconds;
		while (StepFrac >= 1.f)
		{
			StepFrac -= 1.f;
			Step = (Step + 1) % StepCycle;
		}
	}
	IdleTime = Lengths[CLIP_IDLE] > 0.f ? FMath::Fmod(IdleTime + DeltaSeconds, Lengths[CLIP_IDLE]) : 0.f;

	Times[CLIP_IDLE] = IdleTime;
	for (int32 I = CLIP_FWD; I <= CLIP_RIGHT; ++I)
	{
		if (!Clips[I]) continue;
		float Start, End;
		StepSpan(I, Start, End);
		Times[I] = FMath::Fmod(FMath::Lerp(Start, End, StepFrac), Lengths[I]);
	}

	if (bAimJump)
	{
		// 시선 위아래 각은 Lyra 원본 애니가 읽는 값(GetBaseAimRotation)과 같은 것을 쓴다
		const APawn* Pawn = Cast<APawn>(Owner);
		Pitch = Pawn ? FMath::Clamp(FRotator::NormalizeAxis(Pawn->GetBaseAimRotation().Pitch), -90.f, 90.f) : 0.f;
		const ACharacter* Character = Cast<ACharacter>(Owner);
		const UCharacterMovementComponent* Move = Character ? Character->GetCharacterMovement() : nullptr;
		const bool bFalling = Move && Move->IsFalling();
		// 이동 입력이 있나 — Lyra 애니의 HasAcceleration 과 같은 값(서버는 받은 입력으로 안다)
		const bool bAccelerating = Move && !FMath::IsNearlyZero(Move->GetCurrentAcceleration().SizeSquared2D());
		AimBlend = FMath::FInterpTo(AimBlend, bAccelerating ? 0.f : 1.f, DeltaSeconds, AIM_BLEND_SPEED);
		// 꼭대기까지 남은 시간 — Lyra 애니의 TimeToJumpApex 와 같은 식
		const float Gravity = Move ? -Move->GetGravityZ() : 0.f;
		const float TimeToApex = Velocity.Z > 0.0 && Gravity > UE_SMALL_NUMBER ? (float)Velocity.Z / Gravity : 0.f;
		const float GroundDistance = bFalling ? MeasureGroundDistance(Character, Move) : 0.f;

		// 땅에서 이동 입력을 놓은 순간 — 서기 클립이 STOP_CLIP_SECONDS 뒤 0초를 지나게 옮긴다(위 상수 설명)
		if (!bFalling && bWasAccelerating && !bAccelerating && Lengths[CLIP_IDLE] > STOP_CLIP_SECONDS) IdleTime = Lengths[CLIP_IDLE] - STOP_CLIP_SECONDS;
		bWasAccelerating = bAccelerating;

		// 발 디딤 내림 — 땅에선 잰 거리만큼, 뜬 직후엔 마지막으로 잰 거리만큼 내린다. bWasFalling 은 아직 지난 프레임 값이다
		if (bFalling)
		{
			AirTime = bWasFalling ? AirTime + DeltaSeconds : 0.f;
			GroundTime = 0.f;
		}
		else
		{
			GroundTime = bWasFalling ? 0.f : GroundTime + DeltaSeconds;
			float Gap = 0.f, FloorDistance = 0.f;
			if (MeasurePlantGap(Character, Move, Gap, FloorDistance))
			{
				if (!bPlantLogged)		// 내려앉을 때마다 한 줄 — Lyra 실행값(3.97 · 4.15)과 대조한다
				{
					UE_LOG(LogMOBSimple, Log, TEXT("MOB_SIMPLE_PLANT gap=%.2f floorDistance=%.2f"), Gap, FloorDistance);
					bPlantLogged = true;
				}
				PlantDrop = Gap;
			}
		}
		if (bFalling) bPlantLogged = false;
		const bool bPlant = bFalling ? AirTime < PLANT_RELEASE_SECONDS - PLANT_TIME_SLACK : GroundTime >= PLANT_LAND_DELAY - PLANT_TIME_SLACK;
		const USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
		PlantOffset = bPlant && Mesh ? Mesh->GetComponentTransform().InverseTransformVectorNoScale(FVector(0.0, 0.0, -PlantDrop)) : FVector::ZeroVector;

		UpdateJump(bFalling, bAccelerating, (float)Velocity.Z, TimeToApex, GroundDistance, DeltaSeconds);
		UpdateIdleBreak(!bFalling && !bAccelerating && Speed < 1.f && JumpCount == 0, DeltaSeconds);
	}
}

bool FMOBSimpleAnimProxy::Evaluate(FPoseContext& Output)
{
	// 서기 자리는 대기 몸짓이 있으면 그 몫만큼 나눠 준다(FIDGET_SLOT)
	constexpr int32 FIDGET_SLOT = kClips;
	const float Fidget = FidgetAlpha();
	int32 Active[kClips + 1];
	TArray<float, TInlineAllocator<kClips + 1>> ActiveWeights;
	for (int32 I = 0; I < kClips; ++I)
	{
		const float Weight = I == CLIP_IDLE ? Weights[I] * (1.f - Fidget) : Weights[I];
		if (Clips[I] && Weight > ZERO_ANIMWEIGHT_THRESH)
		{
			Active[ActiveWeights.Num()] = I;
			ActiveWeights.Add(Weight);
		}
	}
	if (Clips[CLIP_IDLE] && Weights[CLIP_IDLE] * Fidget > ZERO_ANIMWEIGHT_THRESH)
	{
		Active[ActiveWeights.Num()] = FIDGET_SLOT;
		ActiveWeights.Add(Weights[CLIP_IDLE] * Fidget);
	}
	const int32 Count = ActiveWeights.Num();
	if (Count == 0)
	{
		Output.ResetToRefPose();
		return true;
	}

	TArray<FCompactPose, TInlineAllocator<kClips + 1>> Poses;
	TArray<FBlendedCurve, TInlineAllocator<kClips + 1>> Curves;
	TArray<UE::Anim::FStackAttributeContainer, TInlineAllocator<kClips + 1>> Attributes;
	Poses.SetNum(Count, EAllowShrinking::No);
	Curves.SetNum(Count, EAllowShrinking::No);
	Attributes.SetNum(Count, EAllowShrinking::No);
	for (int32 N = 0; N < Count; ++N)
	{
		const int32 I = Active[N];
		FPoseContext ClipContext(Output);
		FAnimationPoseData ClipData(ClipContext);
		if (I == FIDGET_SLOT) FidgetClip->GetAnimationPose(ClipData, FAnimExtractContext((double)FMath::Min(FidgetTime, FidgetLength), false, FDeltaTimeRecord(), false));
		else Clips[I]->GetAnimationPose(ClipData, FAnimExtractContext((double)Times[I], false, FDeltaTimeRecord(), true));
		Poses[N].MoveBonesFrom(ClipContext.Pose);
		Curves[N].MoveFrom(ClipContext.Curve);
		Attributes[N].MoveFrom(ClipContext.CustomAttributes);
	}

	FAnimationPoseData OutputData(Output);
	FAnimationRuntime::BlendPosesTogether(Poses, Curves, Attributes, ActiveWeights, OutputData);

	if (bAimJump)
	{
		// 차례 = 걸음 · 점프 → 착지 충격 덧붙임 → 조준 보정. Lyra 그래프의 차례는 열어 보지 않았다(둘 다 덧붙임이라 바뀌어도 차이는 작다)
		ApplyJump(Output);
		ApplyRecovery(Output);
		ApplyAim(Output);
		ApplyHandIK(Output);	// 실행값: 손은 IK 총 뼈에 붙고(공중 거리 0.0), 그 뒤 발 디딤 내림이 손까지 함께 내린다(땅 거리 4.0)
	}

	// 위치는 서버 값이다 — 클립의 뿌리 이동이 몸을 끌고 가지 않게 뿌리를 제자리에 묶는다
	const FCompactPoseBoneIndex RootBone(0);
	Output.Pose[RootBone] = Output.Pose.GetRefPose(RootBone);
	if (bAimJump) ApplyPlant(Output);
	return true;
}

// IK 총 뼈를 오른손 자리로 옮기고, 왼손을 IK 왼손 뼈(총 뼈에 붙어 같이 옮겨진 자리)로 당긴다.
// 팔꿈치는 지금 굽은 면 그대로, 손 방향도 그대로 — 손 방향까지 IK 뼈를 따르게 하면 Lyra 와 멀어졌다(착지 뒤 서 있는 구간 오른손 방향 차이 0.1도 → 2.3도)
void FMOBSimpleAnimProxy::ApplyHandIK(FPoseContext& Output) const
{
	const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
	const FCompactPoseBoneIndex HandR = FindCompactBone(Bones, HAND_R);
	const FCompactPoseBoneIndex Gun = FindCompactBone(Bones, IK_GUN);
	const FCompactPoseBoneIndex IkLeft = FindCompactBone(Bones, IK_HAND_L);
	const FCompactPoseBoneIndex Upper = FindCompactBone(Bones, LEFT_ARM[0]);
	const FCompactPoseBoneIndex Lower = FindCompactBone(Bones, LEFT_ARM[1]);
	const FCompactPoseBoneIndex Hand = FindCompactBone(Bones, LEFT_ARM[2]);
	if (!HandR.IsValid() || !Gun.IsValid() || !IkLeft.IsValid() || !Upper.IsValid() || !Lower.IsValid() || !Hand.IsValid()) return;
	FCSPose<FCompactPose> Pose;
	Pose.InitPose(Output.Pose);
	const FTransform GunCS = Pose.GetComponentSpaceTransform(Gun);
	const FTransform IkLeftInGun = Pose.GetComponentSpaceTransform(IkLeft).GetRelativeTransform(GunCS);
	const FTransform NewGunCS = Pose.GetComponentSpaceTransform(HandR);
	const FTransform NewIkLeftCS = IkLeftInGun * NewGunCS;
	FTransform UpperCS = Pose.GetComponentSpaceTransform(Upper);
	FTransform LowerCS = Pose.GetComponentSpaceTransform(Lower);
	FTransform HandCS = Pose.GetComponentSpaceTransform(Hand);
	AnimationCore::SolveTwoBoneIK(UpperCS, LowerCS, HandCS, LowerCS.GetLocation(), NewIkLeftCS.GetLocation(), false, 1.0, 1.0);
	TArray<FBoneTransform, TInlineAllocator<4>> Solved;
	Solved.Emplace(Gun, NewGunCS);
	Solved.Emplace(Upper, UpperCS);
	Solved.Emplace(Lower, LowerCS);
	Solved.Emplace(Hand, HandCS);
	Solved.Sort(FCompareBoneTransformIndex());
	Pose.LocalBlendCSBoneTransforms(Solved, 1.f);
	FCSPose<FCompactPose>::ConvertComponentPosesToLocalPoses(MoveTemp(Pose), Output.Pose);
}

// 골반을 PlantOffset 만큼 옮긴다 — 골반 아래가 다 따라 내려가고, 뿌리에 매달린 IK 뼈는 그대로다(Lyra 실행값과 같은 모양)
void FMOBSimpleAnimProxy::ApplyPlant(FPoseContext& Output) const
{
	if (PlantOffset.IsNearlyZero()) return;
	const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
	const FCompactPoseBoneIndex Pelvis = FindCompactBone(Bones, TEXT("pelvis"));
	if (!Pelvis.IsValid() || Bones.GetParentBoneIndex(Pelvis) != FCompactPoseBoneIndex(0)) return;	// 골반 부모가 뿌리일 때만 — 그래야 뿌리 기준이 곧 몸 기준이다
	Output.Pose[Pelvis].AddToTranslation(Output.Pose[FCompactPoseBoneIndex(0)].GetRotation().UnrotateVector(PlantOffset));
}

void MOBApplySimpleAnim(ACharacter* Character)
{
	USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	if (!Mesh)
	{
		UE_LOG(LogMOBSimple, Error, TEXT("MOB_SIMPLE_FAIL no mesh character=%s"), *GetNameSafe(Character));
		return;
	}
	Mesh->SetAnimInstanceClass(UMOBSimpleAnimInstance::StaticClass());
	UE_LOG(LogMOBSimple, Log, TEXT("MOB_SIMPLE_APPLIED character=%s anim=%s"), *GetNameSafe(Character), *GetNameSafe(Mesh->GetAnimInstance()));
}

bool MOBGetSimpleAnimState(const ACharacter* Character, float OutWeights[FMOBSimpleAnimProxy::kClips], float OutTimes[FMOBSimpleAnimProxy::kClips])
{
	const USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	UMOBSimpleAnimInstance* Instance = Mesh ? Cast<UMOBSimpleAnimInstance>(Mesh->GetAnimInstance()) : nullptr;
	if (!Instance) return false;
	Instance->GetPlaybackState(OutWeights, OutTimes);
	return true;
}

bool MOBDumpSimpleClips(const ACharacter* Character, int32 SampleHz, FString& Out)
{
	const USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	UMOBSimpleAnimInstance* Instance = Mesh ? Cast<UMOBSimpleAnimInstance>(Mesh->GetAnimInstance()) : nullptr;
	return Instance && Instance->DumpClips(SampleHz, Out);
}

bool MOBGetSimpleAimJump(const ACharacter* Character, float& OutPitch, int32& OutJumpClip, float& OutJumpWeight)
{
	const USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	UMOBSimpleAnimInstance* Instance = Mesh ? Cast<UMOBSimpleAnimInstance>(Mesh->GetAnimInstance()) : nullptr;
	if (!Instance) return false;
	Instance->GetAimJump(OutPitch, OutJumpClip, OutJumpWeight);
	return true;
}

void UMOBSimpleAnimInstance::GetAimJump(float& OutPitch, int32& OutJumpClip, float& OutJumpWeight)
{
	GetProxyOnGameThread<FMOBSimpleAnimProxy>().GetAimJump(OutPitch, OutJumpClip, OutJumpWeight);
}

void UMOBSimpleAnimInstance::GetPlaybackState(float OutWeights[FMOBSimpleAnimProxy::kClips], float OutTimes[FMOBSimpleAnimProxy::kClips])
{
	GetProxyOnGameThread<FMOBSimpleAnimProxy>().GetState(OutWeights, OutTimes);
}

bool UMOBSimpleAnimInstance::DumpClips(int32 SampleHz, FString& Out)
{
	const USkeletalMeshComponent* Mesh = GetSkelMeshComponent();
	const UPhysicsAsset* PA = Mesh ? Mesh->GetPhysicsAsset() : nullptr;
	if (!PA || SampleHz <= 0) return false;

	// 판정 모양이 붙은 뼈와 그 조상만 남긴다 — 손가락·얼굴 뼈까지 뽑으면 파일만 커진다
	const FMOBSimpleAnimProxy& Proxy = GetProxyOnGameThread<FMOBSimpleAnimProxy>();
	const FBoneContainer& Bones = Proxy.GetRequiredBones();
	TSet<int32> Keep;
	for (const USkeletalBodySetup* B : PA->SkeletalBodySetups)
	{
		if (!B) continue;
		const int32 MeshIndex = Mesh->GetBoneIndex(B->BoneName);
		if (MeshIndex == INDEX_NONE) continue;
		FCompactPoseBoneIndex Index = Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshIndex));
		while (Index.GetInt() != INDEX_NONE)
		{
			Keep.Add(Index.GetInt());
			if (Index.IsRootBone()) break;
			Index = Bones.GetParentBoneIndex(Index);
		}
	}
	Proxy.AppendClipDump(Out, SampleHz, Keep);
	return true;
}

void MOBResetSimpleAnim(ACharacter* Character)
{
	const USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	if (UMOBSimpleAnimInstance* Instance = Mesh ? Cast<UMOBSimpleAnimInstance>(Mesh->GetAnimInstance()) : nullptr)
	{
		Instance->ResetPlayback();
	}
}

void MOBGetSimpleIdleBreak(float& OutDelay, float& OutAfterStop)
{
	OutDelay = IDLE_BREAK_DELAY;
	OutAfterStop = STOP_CLIP_SECONDS + IDLE_BREAK_DELAY;
}
