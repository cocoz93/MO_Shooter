#include "MOBAnimCompareSubsystem.h"

#include "AIController.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Animation/AnimClassInterface.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimNode_StateMachine.h"
#include "Animation/AnimStateMachineTypes.h"
#include "BrainComponent.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameEngine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Equipment/LyraEquipmentDefinition.h"
#include "Equipment/LyraEquipmentInstance.h"
#include "Equipment/LyraEquipmentManagerComponent.h"
#include "FrameGrabber.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "MOBSimpleAnimInstance.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "Slate/SceneViewport.h"
#include "Weapons/LyraWeaponSpawner.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MOBAnimCompareSubsystem)

DEFINE_LOG_CATEGORY_STATIC(LogMOBAComp, Log, All);

namespace
{
	// 경로 한 조각. Angle = 몸 앞 기준 이동 방향(+90 오른쪽). AngleRate·YawRate = 초당 돌리는 각도
	// PitchFrom → PitchTo = 조각 동안 시선 위아래 각(+ 위). bJump = 조각 처음에 뛴다
	struct FScriptSegment
	{
		const TCHAR* Name;
		float Seconds;
		bool bMove;
		float Angle;
		float AngleRate;
		float YawRate;
		float PitchFrom = 0.f;
		float PitchTo = 0.f;
		bool bJump = false;
	};

	// 사람이 눈으로 볼 동작을 한 번씩 — 서기·출발·멈춤·옆·뒤·급한 방향 바꿈·모든 방향·제자리 회전. 길이는 제안값이다(한 동작을 알아볼 만큼)
	const FScriptSegment SCRIPT[] = {
		{ TEXT("idle"),   2.0f, false,    0.f,  0.f,  0.f },
		{ TEXT("fwd"),    2.0f, true,     0.f,  0.f,  0.f },
		{ TEXT("stop1"),  1.5f, false,    0.f,  0.f,  0.f },
		{ TEXT("left"),   1.5f, true,   -90.f,  0.f,  0.f },
		{ TEXT("right"),  1.5f, true,    90.f,  0.f,  0.f },
		{ TEXT("stop2"),  1.0f, false,    0.f,  0.f,  0.f },
		{ TEXT("bwd"),    2.0f, true,   180.f,  0.f,  0.f },
		{ TEXT("flip1"),  0.8f, true,     0.f,  0.f,  0.f },
		{ TEXT("flip2"),  0.8f, true,   180.f,  0.f,  0.f },
		{ TEXT("flip3"),  0.8f, true,     0.f,  0.f,  0.f },
		{ TEXT("stop3"),  1.0f, false,    0.f,  0.f,  0.f },
		{ TEXT("circle"), 4.0f, true,     0.f, 90.f,  0.f },
		{ TEXT("stop4"),  1.0f, false,    0.f,  0.f,  0.f },
		{ TEXT("turn"),   3.0f, false,    0.f,  0.f, 90.f },
		{ TEXT("idle2"),  1.0f, false,    0.f,  0.f,  0.f },
	};
	constexpr int32 NUM_SEGMENTS = UE_ARRAY_COUNT(SCRIPT);

	// 2차 화면 시험(-MOBACScript=aimjump) — 위아래 조준과 점프. 각도 ±60 은 사람이 위아래를 겨누는 흔한 폭으로 고른 제안값이다
	const FScriptSegment AIMJUMP_SCRIPT[] = {
		{ TEXT("idle"),      1.5f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false },
		{ TEXT("look_up"),   1.5f, false,   0.f, 0.f, 0.f,   0.f,  60.f, false },
		{ TEXT("hold_up"),   1.0f, false,   0.f, 0.f, 0.f,  60.f,  60.f, false },
		{ TEXT("look_down"), 2.0f, false,   0.f, 0.f, 0.f,  60.f, -60.f, false },
		{ TEXT("hold_down"), 1.0f, false,   0.f, 0.f, 0.f, -60.f, -60.f, false },
		{ TEXT("look_mid"),  1.0f, false,   0.f, 0.f, 0.f, -60.f,   0.f, false },
		{ TEXT("run_up"),    1.5f, true,    0.f, 0.f, 0.f,   0.f,  45.f, false },
		{ TEXT("run_down"),  1.5f, true,  180.f, 0.f, 0.f,  45.f, -45.f, false },
		{ TEXT("stop_mid"),  1.0f, false,   0.f, 0.f, 0.f, -45.f,   0.f, false },
		{ TEXT("jump"),      1.6f, false,   0.f, 0.f, 0.f,   0.f,   0.f, true  },
		{ TEXT("idle2"),     1.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false },
		{ TEXT("run_pre"),   0.8f, true,    0.f, 0.f, 0.f,   0.f,   0.f, false },
		{ TEXT("run_jump"),  1.6f, true,    0.f, 0.f, 0.f,   0.f,   0.f, true  },
		{ TEXT("stop_end"),  1.5f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false },
	};
	constexpr int32 NUM_AIMJUMP_SEGMENTS = UE_ARRAY_COUNT(AIMJUMP_SCRIPT);

	// 3차 화면 시험(-MOBACScript=startstop) — 네 방향 출발 · 멈춤 · 돌아서기. 지금 기록에 왼쪽 · 오른쪽 클립이 없어 새로 찍는다.
	// 달리기 2.5초 = 기록에서 출발 클립이 1.6초까지 돌았으므로 달리기로 다 넘어가는 데까지. 멈춤 2초 = 멈춤 클립 최대 1.81초보다 길고 대기 몸짓(6초 뒤)보다 짧게.
	// 돌아서기 1.5초 = 기록(run_down)에서 방향 전환 클립이 도는 시간. 좌우 0.4초 = 짧게 번갈아 움직일 때 방향 전환이 끊기는 모습을 보려는 제안값
	const FScriptSegment STARTSTOP_SCRIPT[] = {
		{ TEXT("idle"),       2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("fwd"),        2.5f, true,    0.f, 0.f, 0.f },
		{ TEXT("stop_fwd"),   2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("bwd"),        2.5f, true,  180.f, 0.f, 0.f },
		{ TEXT("stop_bwd"),   2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("left"),       2.5f, true,  -90.f, 0.f, 0.f },
		{ TEXT("stop_left"),  2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("right"),      2.5f, true,   90.f, 0.f, 0.f },
		{ TEXT("stop_right"), 2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("diag"),       2.5f, true,   45.f, 0.f, 0.f },
		{ TEXT("stop_diag"),  2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("piv_bf_a"),   1.5f, true,  180.f, 0.f, 0.f },
		{ TEXT("piv_bf_b"),   1.5f, true,    0.f, 0.f, 0.f },
		{ TEXT("stop_bf"),    2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("piv_fb_a"),   1.5f, true,    0.f, 0.f, 0.f },
		{ TEXT("piv_fb_b"),   1.5f, true,  180.f, 0.f, 0.f },
		{ TEXT("stop_fb"),    2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("piv_lr_a"),   1.5f, true,  -90.f, 0.f, 0.f },
		{ TEXT("piv_lr_b"),   1.5f, true,   90.f, 0.f, 0.f },
		{ TEXT("stop_lr"),    2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("piv_rl_a"),   1.5f, true,   90.f, 0.f, 0.f },
		{ TEXT("piv_rl_b"),   1.5f, true,  -90.f, 0.f, 0.f },
		{ TEXT("stop_rl"),    2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("ad1"),        0.4f, true,  -90.f, 0.f, 0.f },
		{ TEXT("ad2"),        0.4f, true,   90.f, 0.f, 0.f },
		{ TEXT("ad3"),        0.4f, true,  -90.f, 0.f, 0.f },
		{ TEXT("ad4"),        0.4f, true,   90.f, 0.f, 0.f },
		{ TEXT("ad5"),        0.4f, true,  -90.f, 0.f, 0.f },
		{ TEXT("ad6"),        0.4f, true,   90.f, 0.f, 0.f },
		{ TEXT("stop_ad"),    2.0f, false,   0.f, 0.f, 0.f },
	};
	constexpr int32 NUM_STARTSTOP_SEGMENTS = UE_ARRAY_COUNT(STARTSTOP_SCRIPT);
	constexpr double AIM_MARK_DISTANCE = 1000.0;	// 시선 표적을 둘 거리. 멀수록 상대가 움직여도 각이 덜 흔들린다
	constexpr float JUMP_PRESS_SECONDS = 0.1f;		// 뛰기 입력을 누르고 있는 시간 — 계속 누르면 내려앉자마자 또 뛴다

	const FScriptSegment& Seg(EMOBACScript Kind, int32 I)
	{
		switch (Kind)
		{
		case EMOBACScript::AimJump: return AIMJUMP_SCRIPT[I];
		case EMOBACScript::StartStop: return STARTSTOP_SCRIPT[I];
		case EMOBACScript::Run: break;
		}
		return SCRIPT[I];
	}
	int32 SegCount(EMOBACScript Kind)
	{
		switch (Kind)
		{
		case EMOBACScript::AimJump: return NUM_AIMJUMP_SEGMENTS;
		case EMOBACScript::StartStop: return NUM_STARTSTOP_SEGMENTS;
		case EMOBACScript::Run: break;
		}
		return NUM_SEGMENTS;
	}

	// 두 번 찍는다 — 가까이(결함이 잘 보임) · 실제 교전 거리쯤(사람이 평소 보는 크기). 거리는 제안값이다
	constexpr int32 NUM_PASSES = 2;
	const double CAMERA_DISTANCE[NUM_PASSES] = { 450.0, 1500.0 };
	const double CAMERA_HEIGHT[NUM_PASSES] = { 40.0, 500.0 };	// 멀리 찍을 땐 주변 경사면에 상대가 가려 위에서 내려다본다
	// 출발 자리는 판마다 같아야 경로·배경이 같다. 내 캐릭터의 시작 자리는 판마다 달라 쓰지 않는다.
	// L_ShooterGym 의 평지 — 이 자리에서 시작한 판은 전 구간 z=142 로 경사면을 안 밟았다(animcmp_simple.csv)
	const FVector FIXED_HOME(-1020.0, -1070.0, 142.0);
	constexpr float FIXED_YAW = 180.f;
	constexpr float CAMERA_FOV = 80.f;			// Lyra 기본 시야(LyraCameraMode 의 FieldOfView 기본값)
	constexpr float FIRST_SETTLE_SECONDS = 4.f;	// 무기 장착·몸 입히기가 끝나고 대기 동작이 자리 잡을 시간
	constexpr float PASS_SETTLE_SECONDS = 1.5f;
	constexpr float IDLE_BREAK_FORCE_TAIL = 0.25f;	// Lyra 대기 몸짓 「남은 시간」에 0 을 넣고 더 덮어 두는 시간(ForceLyraIdleBreak)
	constexpr float MATCH_SETTLE_SECONDS = 3.f;	// 경기 시작 때 캐릭터가 한 번 바뀐다 — 면역이 풀린 채 이만큼 이어져야 그 캐릭터로 찍는다(MOBWeaponRecordSubsystem 과 같은 절차)
	constexpr int32 CAPTURE_EVERY = 2;			// 60프레임 중 하나 걸러 30fps — 720p 원시 영상이 한 판 약 5GB 로 끝나는 간격
	constexpr double WAIT_LIMIT_SECONDS = 120.0;
	constexpr double DEDI_WAIT_LIMIT_SECONDS = 420.0;	// 데디 서버는 클라가 붙고 경기가 시작될 때까지 기다린다. 에디터 실행 파일이 맵을 읽는 시간까지 들어간다(제안값)

	UAbilitySystemComponent* FindASC(AActor* A)
	{
		return A ? UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(A) : nullptr;
	}
}

bool UMOBAnimCompareSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* W = Cast<UWorld>(Outer);
	if (!W || !W->IsGameWorld()) return false;
	FString T;
	return FParse::Value(FCommandLine::Get(), TEXT("MOBAnimCompare="), T);
}

void UMOBAnimCompareSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	const TCHAR* Cmd = FCommandLine::Get();
	FParse::Value(Cmd, TEXT("MOBAnimCompare="), Tag);
	bSimple = FParse::Param(Cmd, TEXT("MOBSimpleAnim"));
	bVideo = !FParse::Param(Cmd, TEXT("MOBACNoVideo"));
	bShapes = FParse::Param(Cmd, TEXT("MOBACShapes"));
	bDumpClips = FParse::Param(Cmd, TEXT("MOBACDumpClips"));
	FString ScriptName;
	FParse::Value(Cmd, TEXT("MOBACScript="), ScriptName);
	Script = ScriptName == TEXT("aimjump") ? EMOBACScript::AimJump : ScriptName == TEXT("startstop") ? EMOBACScript::StartStop : EMOBACScript::Run;
	bAimJump = Script != EMOBACScript::Run;
	bProbe = FParse::Param(Cmd, TEXT("MOBACProbe"));
	if (bShapes || bDumpClips) PhysWeak = LoadObject<UPhysicalMaterial>(nullptr, TEXT("/Game/Characters/Heroes/PhysMat_Player_WeakSpot.PhysMat_Player_WeakSpot"));
	GAreScreenMessagesEnabled = false;	// 봇이 그전에 쏠 때 남은 오류 글자가 화면을 가린다
	FMath::RandInit(20261105);		// Lyra 는 캐릭터마다 몸을 무작위로 고른다 — 판끼리 몸을 맞추려는 시도. 실제로 맞았는지는 MOB_ACOMP ready 줄로 본다
	// w0~w4·t0~t4 = 단순 재생이 그 프레임 자세에 쓴 클립 가중치와 클립 시각(Lyra 원본 판은 0). 언리얼 밖 계산이 같은 상태에서 출발하게 하려고 남긴다
	Csv = TEXT("pass,frame,segment,segTime,x,y,z,velX,velY,yaw,headX,headY,headZ,pelvisX,pelvisY,pelvisZ,w0,w1,w2,w3,w4,t0,t1,t2,t3,t4,fidget,recovery,aimBlend,pitch,plantX,plantY,plantZ,layers,c0,tm0,f0,c1,tm1,f1,c2,tm2,f2,c3,tm3,f3,reset,seq\n");
}

TStatId UMOBAnimCompareSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMOBAnimCompareSubsystem, STATGROUP_Tickables);
}

bool UMOBAnimCompareSubsystem::GetRunPosition(int32& OutPass, const TCHAR*& OutSegment, float& OutSegmentTime) const
{
	if (Stage != EStage::Run) return false;
	OutPass = Pass;
	OutSegment = Seg(Script, Segment).Name;
	OutSegmentTime = SegmentTime;
	return true;
}

bool UMOBAnimCompareSubsystem::IsMatchPlaying(float DeltaTime)
{
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	APawn* Me = PC ? PC->GetPawn() : nullptr;
	const UAbilitySystemComponent* ASC = FindASC(Me);
	const FGameplayTag Immune = FGameplayTag::RequestGameplayTag(TEXT("Gameplay.DamageImmunity"));
	if (!ASC || ASC->HasMatchingGameplayTag(Immune) || Me != ClearPawn.Get())
	{
		ClearPawn = Me;
		ClearSeconds = 0.f;
		return false;
	}
	ClearSeconds += DeltaTime;
	return ClearSeconds > MATCH_SETTLE_SECONDS;
}

bool UMOBAnimCompareSubsystem::Prepare()
{
	UWorld* World = GetWorld();
	APlayerController* PC = World->GetFirstPlayerController();
	ACharacter* Me = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
	if (!Me || !FindASC(Me)) return false;

	ACharacter* Target = nullptr;
	for (TActorIterator<ACharacter> It(World); It; ++It)
	{
		if (*It != Me && FindASC(*It))
		{
			Target = *It;
			break;
		}
	}
	if (!Target) return false;

	// 봇 머리를 끈다 — 몸만 빌린다
	if (AAIController* AI = Cast<AAIController>(Target->GetController()))
	{
		if (AI->BrainComponent) AI->BrainComponent->StopLogic(TEXT("MOBAnimCompare"));
		AI->StopMovement();
		AI->ClearFocus(EAIFocusPriority::Gameplay);
		AI->bSetControlRotationFromPawnOrientation = false;
	}

	// 소총으로 바꾼다 — 단순 재생의 클립이 소총 것이다
	const TCHAR* const RiflePath = TEXT("/ShooterCore/Weapons/Rifle/WID_Rifle.WID_Rifle_C");
	UClass* RifleDef = LoadClass<ULyraEquipmentDefinition>(nullptr, RiflePath);
	ULyraEquipmentManagerComponent* EM = Target->FindComponentByClass<ULyraEquipmentManagerComponent>();
	if (!EM || !RifleDef)
	{
		UE_LOG(LogMOBAComp, Error, TEXT("MOB_ACOMP_FAIL rifle em=%d def=%d"), EM ? 1 : 0, RifleDef ? 1 : 0);
		return false;
	}
	const TArray<ULyraEquipmentInstance*> Old = EM->GetEquipmentInstancesOfType(ULyraEquipmentInstance::StaticClass());
	for (ULyraEquipmentInstance* I : Old) EM->UnequipItem(I);
	const ULyraEquipmentInstance* Rifle = EM->EquipItem(RifleDef);

	if (bSimple) MOBApplySimpleAnim(Target);

	// 위아래 시선 — 봇 조종기는 「다른 캐릭터를 볼 때만 고개를 든다」(AAIController::UpdateControlRotation). 그래서 보이지 않는 표적 캐릭터를 시선 자리에 두고 그것을 보게 한다.
	// Lyra 원본 애니와 단순 재생이 같은 값(GetBaseAimRotation)을 읽는다
	if (bAimJump)
	{
		AAIController* AI = Cast<AAIController>(Target->GetController());
		FActorSpawnParameters MarkParams;
		MarkParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		MarkParams.ObjectFlags |= RF_Transient;
		APawn* Mark = AI ? World->SpawnActor<APawn>(APawn::StaticClass(), FTransform::Identity, MarkParams) : nullptr;
		if (!Mark)
		{
			UE_LOG(LogMOBAComp, Error, TEXT("MOB_ACOMP_FAIL aim mark ai=%d"), AI ? 1 : 0);
			return false;
		}
		USceneComponent* MarkRoot = NewObject<USceneComponent>(Mark, TEXT("AimMarkRoot"));
		Mark->SetRootComponent(MarkRoot);
		MarkRoot->RegisterComponent();
		AI->SetFocus(Mark, EAIFocusPriority::Gameplay);
		AimMark = Mark;
	}

	Foe = Target;
	Home = FIXED_HOME;
	StartYaw = FIXED_YAW;
	BodyYaw = StartYaw;
	CameraSide = FRotator(0.f, StartYaw - 90.f, 0.f).Vector();
	Target->TeleportTo(Home, FRotator(0.f, BodyYaw, 0.f), false, true);

	// 무기·회복 발판을 숨기고 끈다 — 몸을 가리고, 밟으면 무기가 바뀔 수 있다. 실제 판도 같은 처리를 한다(MOBridgeSubsystem::TickPickups)
	int32 PadsHidden = 0;
	for (TActorIterator<ALyraWeaponSpawner> It(World); It; ++It)
	{
		It->SetActorHiddenInGame(true);
		It->SetActorEnableCollision(false);
		It->SetActorTickEnabled(false);
		++PadsHidden;
	}

	// 내 캐릭터는 화면에 안 나오고 상대 길도 막지 않게 한다. 충돌을 끄면 바닥을 못 찾아 떨어지므로 움직임부터 끈다
	if (USkeletalMeshComponent* MyMesh = Me->GetMesh()) MyMesh->SetVisibility(false, true);
	if (UCharacterMovementComponent* MyMove = Me->GetCharacterMovement()) MyMove->DisableMovement();
	Me->SetActorEnableCollision(false);

	// 데디 서버에는 화면이 없다 — 카메라와 위젯은 건너뛰고 봇만 움직인다
	if (World->GetNetMode() == NM_DedicatedServer)
	{
		UE_LOG(LogMOBAComp, Log, TEXT("MOB_ACOMP ready tag=%s dedicated=1 foe=%s rifle=%s removedWeapons=%d pads=%d home=(%.0f,%.0f,%.0f) yaw=%.1f"),
			*Tag, *GetNameSafe(Target), *GetNameSafe(Rifle), Old.Num(), PadsHidden, Home.X, Home.Y, Home.Z, BodyYaw);
		return true;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= RF_Transient;
	ACameraActor* Cam = World->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), FTransform::Identity, SpawnParams);
	if (!Cam)
	{
		UE_LOG(LogMOBAComp, Error, TEXT("MOB_ACOMP_FAIL camera"));
		return false;
	}
	Cam->GetCameraComponent()->SetFieldOfView(CAMERA_FOV);
	Cam->GetCameraComponent()->bConstrainAspectRatio = false;
	Camera = Cam;
	PC->bAutoManageActiveCameraTarget = false;
	PC->SetViewTargetWithBlend(Cam, 0.f);

	if (GEngine && GEngine->GameViewport) GEngine->GameViewport->RemoveAllViewportWidgets();	// 조준점·체력바를 뺀다

	UE_LOG(LogMOBAComp, Log, TEXT("MOB_ACOMP ready tag=%s simple=%d video=%d foe=%s rifle=%s removedWeapons=%d pads=%d home=(%.0f,%.0f,%.0f) yaw=%.1f"),
		*Tag, bSimple ? 1 : 0, bVideo ? 1 : 0, *GetNameSafe(Target), *GetNameSafe(Rifle), Old.Num(), PadsHidden, Home.X, Home.Y, Home.Z, BodyYaw);
	return true;
}

void UMOBAnimCompareSubsystem::LogBody() const
{
	// 몸 종류 — 판끼리 몸이 다르면 뼈 길이가 달라 나란히 비교가 틀어진다. 몸은 붙는 데 시간이 걸려 대기가 끝난 뒤 읽는다
	const ACharacter* Target = Foe.Get();
	if (!Target) return;
	FString Body;
	TArray<USkeletalMeshComponent*> Parts;
	Target->GetComponents<USkeletalMeshComponent>(Parts, true);
	for (const USkeletalMeshComponent* Part : Parts) Body += GetNameSafe(Part ? Part->GetSkeletalMeshAsset() : nullptr) + TEXT(" ");
	UE_LOG(LogMOBAComp, Log, TEXT("MOB_ACOMP body anim=%s meshes=[%s]"), *GetNameSafe(Target->GetMesh() ? Target->GetMesh()->GetAnimInstance() : nullptr), *Body);
}

void UMOBAnimCompareSubsystem::BeginPass()
{
	ACharacter* Target = Foe.Get();
	if (!Target) return;
	Segment = 0;
	SegmentTime = 0.f;
	BodyYaw = StartYaw;
	MoveAngle = Seg(Script,0).Angle;
	AimPitch = Seg(Script,0).PitchFrom;
	ResetSegmentStats();
	if (UCharacterMovementComponent* Move = Target->GetCharacterMovement()) Move->StopMovementImmediately();
	Target->TeleportTo(Home, FRotator(0.f, BodyYaw, 0.f), false, true);
	if (bSimple) MOBResetSimpleAnim(Target);
	float Delay = 0.f, AfterStop = 0.f;
	MOBGetSimpleIdleBreak(Delay, AfterStop);
	IdleBreakForceLeft = AfterStop;		// 단순 재생도 판 시작을 「방금 멈췄다」로 보고 이만큼 뒤에 몸짓을 시작한다
	bIdleBreakForcing = !bSimple && Script == EMOBACScript::AimJump;	// startstop 경로는 서 있는 구간이 2초뿐이라 몸짓을 안 띄운다 — 덮으면 첫 멈춤에 Lyra 쪽만 몸짓이 나온다
}

void UMOBAnimCompareSubsystem::ResetSegmentStats()
{
	SegPitchMin = 1e9f;
	SegPitchMax = -1e9f;
	SegSimplePitchMin = 1e9f;
	SegSimplePitchMax = -1e9f;
	SegRiseMax = 0.0;
	SegFallFrames = 0;
	SegJumpWeightMax = 0.f;
	SegJumpClips = 0;
}

// 조각마다 실제로 들어간 값을 남긴다 — 시선 각이 정말 바뀌었는지, 정말 떴는지, 단순 재생이 그 값을 받았는지
void UMOBAnimCompareSubsystem::TrackSegment()
{
	const ACharacter* Target = Foe.Get();
	if (!Target) return;
	const float Pitch = FRotator::NormalizeAxis(Target->GetBaseAimRotation().Pitch);
	SegPitchMin = FMath::Min(SegPitchMin, Pitch);
	SegPitchMax = FMath::Max(SegPitchMax, Pitch);
	SegRiseMax = FMath::Max(SegRiseMax, Target->GetActorLocation().Z - Home.Z);
	const UCharacterMovementComponent* Move = Target->GetCharacterMovement();
	if (Move && Move->IsFalling()) ++SegFallFrames;
	float SimplePitch = 0.f, JumpWeight = 0.f;
	int32 JumpClip = -1;
	if (bSimple && MOBGetSimpleAimJump(Target, SimplePitch, JumpClip, JumpWeight))
	{
		SegSimplePitchMin = FMath::Min(SegSimplePitchMin, SimplePitch);
		SegSimplePitchMax = FMath::Max(SegSimplePitchMax, SimplePitch);
		SegJumpWeightMax = FMath::Max(SegJumpWeightMax, JumpWeight);
		if (JumpClip >= 0) SegJumpClips |= 1 << JumpClip;
	}
}

namespace
{
	// 몸 기준(캡슐 자리 · 몸 방향 요만 뺀 좌표)으로 남길 뼈 — 머리는 판정, 손은 총 방향, 발은 디딤을 본다.
	// root 는 몸 전체가 통째로 밀렸는지, ik_hand_l · ik_hand_gun 은 손 IK 가 손을 어디로 끄는지 보려고 넣었다
	const TCHAR* const PROBE_BONES[] = { TEXT("head"), TEXT("neck_01"), TEXT("spine_05"), TEXT("pelvis"), TEXT("hand_r"), TEXT("hand_l"), TEXT("foot_l"), TEXT("foot_r"), TEXT("ball_l"), TEXT("ball_r"),
		TEXT("root"), TEXT("ik_hand_gun"), TEXT("ik_hand_l"), TEXT("ik_foot_l") };
	const TCHAR* const PROBE_ROT_BONES[] = { TEXT("head"), TEXT("hand_r") };

	FString ProbeInstanceName(const UAnimInstance* Inst)
	{
		FString Name = Inst->GetClass()->GetName();
		Name.RemoveFromEnd(TEXT("_C"));
		return Name;
	}

	bool IsProbeProperty(const FProperty* P)
	{
		const UClass* Owner = P->GetOwnerClass();
		return Owner && (Cast<UBlueprintGeneratedClass>(Owner) || Owner->GetFName() == TEXT("LyraAnimInstance"));
	}

	// 숫자 · 참거짓 · 열거 · FVector · FRotator 만 읽는다. 읽은 칸 수를 돌려준다(0 = 안 읽는 형식)
	int32 ReadProbeProperty(const FProperty* P, const void* Container, double Out[3])
	{
		const void* V = P->ContainerPtrToValuePtr<void>(Container);
		if (const FBoolProperty* B = CastField<FBoolProperty>(P)) { Out[0] = B->GetPropertyValue(V) ? 1.0 : 0.0; return 1; }
		if (const FNumericProperty* N = CastField<FNumericProperty>(P))
		{
			Out[0] = N->IsFloatingPoint() ? N->GetFloatingPointPropertyValue(V) : (double)N->GetSignedIntPropertyValue(V);
			return 1;
		}
		if (const FEnumProperty* E = CastField<FEnumProperty>(P)) { Out[0] = (double)E->GetUnderlyingProperty()->GetSignedIntPropertyValue(V); return 1; }
		if (const FStructProperty* S = CastField<FStructProperty>(P))
		{
			if (S->Struct == TBaseStructure<FVector>::Get()) { const FVector& X = *static_cast<const FVector*>(V); Out[0] = X.X; Out[1] = X.Y; Out[2] = X.Z; return 3; }
			if (S->Struct == TBaseStructure<FRotator>::Get()) { const FRotator& R = *static_cast<const FRotator*>(V); Out[0] = R.Pitch; Out[1] = R.Yaw; Out[2] = R.Roll; return 3; }
		}
		return 0;
	}
}

// Lyra 는 대기 몸짓 지연을 무작위로 뽑는다(실행값 6 · 7 · 10 · 11 · 15초). 단순 재생은 고정값이라, 그대로 두면 판마다 한쪽만 몸짓을 해 화면 대조가 안 된다.
// 판 시작부터 단순 재생이 몸짓을 시작할 때(판 시작 + 멈춤 클립 몫 + 지연)까지 Lyra 의 「남은 시간」 변수를 매 프레임 같은 남은 시간으로 덮는다.
// 한 번만 덮으면 안 된다 — 첫 판은 덮은 뒤 Lyra 가 제 무작위 값으로 다시 채웠고(덮기 전 0), 둘째 판은 서기 상태에 늦게 들어가 2.5초 늦게 나왔다(11-06 실행값)
void UMOBAnimCompareSubsystem::ForceLyraIdleBreak(float DeltaTime)
{
	ACharacter* Target = Foe.Get();
	USkeletalMeshComponent* Mesh = Target ? Target->GetMesh() : nullptr;
	if (!Mesh || !bIdleBreakForcing) return;
	float Delay = 0.f, AfterStop = 0.f;
	MOBGetSimpleIdleBreak(Delay, AfterStop);
	const float Remaining = FMath::Max(IdleBreakForceLeft, 0.f);
	const bool bFirst = IdleBreakForceLeft >= AfterStop;
	int32 Written = 0;
	FString Seen;
	Mesh->ForEachAnimInstance([&](UAnimInstance* Inst)
	{
		const auto Write = [&](const TCHAR* Name, float Value)
		{
			const FNumericProperty* P = CastField<FNumericProperty>(Inst->GetClass()->FindPropertyByName(Name));
			if (!P || !P->IsFloatingPoint()) return;
			void* V = P->ContainerPtrToValuePtr<void>(Inst);
			Seen += FString::Printf(TEXT(" %s.%s=%.3f"), *ProbeInstanceName(Inst), Name, P->GetFloatingPointPropertyValue(V));
			P->SetFloatingPointPropertyValue(V, (double)Value);
			++Written;
		};
		Write(TEXT("IdleBreakDelayTime"), Delay);
		Write(TEXT("TimeUntilNextIdleBreak"), Remaining);
	});
	if (Written != 2) UE_LOG(LogMOBAComp, Error, TEXT("MOB_ACOMP FAIL idle_break_force written=%d (expected 2)"), Written);
	if (bFirst) UE_LOG(LogMOBAComp, Log, TEXT("MOB_ACOMP idle_break_force begin pass=%d remaining=%.3f was=[%s ]"), Pass, Remaining, *Seen);
	IdleBreakForceLeft -= DeltaTime;
	// 0 을 넣은 뒤 IDLE_BREAK_FORCE_TAIL 만큼 더 둔다 — 그사이 Lyra 가 서기 상태에서 0 을 보고 몸짓을 고른다
	if (IdleBreakForceLeft < -IDLE_BREAK_FORCE_TAIL)
	{
		UE_LOG(LogMOBAComp, Log, TEXT("MOB_ACOMP idle_break_force end pass=%d last=[%s ]"), Pass, *Seen);
		bIdleBreakForcing = false;
	}
}

void UMOBAnimCompareSubsystem::BuildProbe()
{
	bProbeBuilt = true;
	ACharacter* Target = Foe.Get();
	USkeletalMeshComponent* Mesh = Target ? Target->GetMesh() : nullptr;
	if (!Mesh) return;
	FString Header = TEXT("pass,frame,segment,segTime");
	for (const TCHAR* Bone : PROBE_BONES) Header += FString::Printf(TEXT(",%s.x,%s.y,%s.z"), Bone, Bone, Bone);
	for (const TCHAR* Bone : PROBE_ROT_BONES) Header += FString::Printf(TEXT(",%s.qx,%s.qy,%s.qz,%s.qw"), Bone, Bone, Bone, Bone);
	FString Instances;
	FString MachineHeader;
	Mesh->ForEachAnimInstance([&](UAnimInstance* Inst)
	{
		const FString InstName = ProbeInstanceName(Inst);
		Instances += InstName + TEXT(" ");
		for (TFieldIterator<FProperty> It(Inst->GetClass()); It; ++It)
		{
			double Dummy[3];
			if (!IsProbeProperty(*It) || ReadProbeProperty(*It, Inst, Dummy) == 0) continue;
			const int32 Count = ReadProbeProperty(*It, Inst, Dummy);
			const FString Name = (InstName + TEXT(".") + It->GetName()).Replace(TEXT(","), TEXT(" "));
			if (Count == 1) Header += TEXT(",") + Name;
			else Header += FString::Printf(TEXT(",%s.0,%s.1,%s.2"), *Name, *Name, *Name);
			ProbeVars.Add({ Inst, *It });
		}
		if (const IAnimClassInterface* AC = IAnimClassInterface::GetFromClass(Inst->GetClass()))
		{
			const TArray<FBakedAnimationStateMachine>& Machines = AC->GetBakedStateMachines();
			for (int32 M = 0; M < Machines.Num(); ++M)
			{
				MachineHeader += FString::Printf(TEXT(",sm:%s.%s"), *InstName, *Machines[M].MachineName.ToString().Replace(TEXT(","), TEXT(" ")));
				ProbeMachines.Add({ Inst, Machines[M].MachineName });
			}
		}
	});
	// 줄은 변수 전부 → 상태 기계 전부 차례로 쓴다(WriteProbe). 제목도 같은 차례로
	Header += MachineHeader + TEXT(",montage,players,curves\n");
	ProbeCsv = Header;
	UE_LOG(LogMOBAComp, Log, TEXT("MOB_ACOMP probe_build vars=%d machines=%d instances=[%s]"), ProbeVars.Num(), ProbeMachines.Num(), *Instances);
}

void UMOBAnimCompareSubsystem::WriteProbe(float SegTime)
{
	if (!bProbeBuilt) BuildProbe();
	const ACharacter* Target = Foe.Get();
	const USkeletalMeshComponent* Mesh = Target ? Target->GetMesh() : nullptr;
	if (!Mesh) return;
	const FVector Loc = Target->GetActorLocation();
	const FQuat ActorInv = FRotator(0, Target->GetActorRotation().Yaw, 0).Quaternion().Inverse();
	FString Row = FString::Printf(TEXT("%d,%d,%s,%.3f"), Pass, Frame, Seg(Script,Segment).Name, SegTime);
	for (const TCHAR* Bone : PROBE_BONES)
	{
		const FVector P = ActorInv.RotateVector(Mesh->GetBoneLocation(Bone) - Loc);
		Row += FString::Printf(TEXT(",%.3f,%.3f,%.3f"), P.X, P.Y, P.Z);
	}
	for (const TCHAR* Bone : PROBE_ROT_BONES)
	{
		const FQuat Q = ActorInv * Mesh->GetBoneQuaternion(Bone);
		Row += FString::Printf(TEXT(",%.6f,%.6f,%.6f,%.6f"), Q.X, Q.Y, Q.Z, Q.W);
	}
	for (const FProbeVar& Var : ProbeVars)
	{
		const UAnimInstance* Inst = Var.Instance.Get();
		double Value[3] = {};
		const int32 Count = Inst ? ReadProbeProperty(Var.Prop, Inst, Value) : 0;
		if (Count == 3) Row += FString::Printf(TEXT(",%.4g,%.4g,%.4g"), Value[0], Value[1], Value[2]);
		else Row += FString::Printf(TEXT(",%.4g"), Value[0]);
	}
	for (const FProbeMachine& M : ProbeMachines)
	{
		const UAnimInstance* Inst = M.Instance.Get();
		const FAnimNode_StateMachine* Node = Inst ? Inst->GetStateMachineInstanceFromName(M.Name) : nullptr;
		Row += TEXT(",") + (Node ? Node->GetCurrentStateName().ToString().Replace(TEXT(","), TEXT(" ")) : FString());
	}
	UAnimInstance* Main = Mesh->GetAnimInstance();
	Row += TEXT(",") + GetNameSafe(Main ? Main->GetCurrentActiveMontage() : nullptr);
	// 지금 재생 중인 클립 — 「클립@시각*가중치」. Lyra 가 실제로 어느 클립을 트는지(몸에 따라 다른 클립을 쓰는지) 보려고
	FString Players;
	const auto AddPlayers = [&Players](const TArray<FAnimTickRecord>& Records)
	{
		for (const FAnimTickRecord& R : Records)
		{
			if (R.EffectiveBlendWeight < 0.01f || !R.SourceAsset) continue;
			Players += FString::Printf(TEXT("%s@%.3f*%.2f;"), *R.SourceAsset->GetName(), R.TimeAccumulator ? *R.TimeAccumulator : -1.f, R.EffectiveBlendWeight);
		}
	};
	const_cast<USkeletalMeshComponent*>(Mesh)->ForEachAnimInstance([&](UAnimInstance* Inst)
	{
		for (const TPair<FName, FAnimGroupInstance>& G : Inst->GetSyncGroupMapRead()) AddPlayers(G.Value.ActivePlayers);
		AddPlayers(Inst->GetUngroupedActivePlayersRead());
	});
	if (bSimple) MOBGetSimplePlayers(Target, Players);	// 단순 재생은 엔진 재생 기록을 안 남겨 직접 받는다
	Row += TEXT(",") + Players;
	FString Curves;
	if (Main)
	{
		TArray<FName> Names;
		Main->GetActiveCurveNames(EAnimCurveType::AttributeCurve, Names);
		for (const FName& Name : Names)
		{
			const float Value = Main->GetCurveValue(Name);
			if (FMath::Abs(Value) > 1e-4f) Curves += FString::Printf(TEXT("%s=%.3f;"), *Name.ToString().Replace(TEXT(","), TEXT(" ")), Value);
		}
	}
	Row += TEXT(",") + Curves + TEXT("\n");
	ProbeCsv += Row;
}

void UMOBAnimCompareSubsystem::LogSegment() const
{
	if (!bAimJump) return;
	const FScriptSegment& S = Seg(Script,Segment);
	UE_LOG(LogMOBAComp, Log, TEXT("MOB_ACOMP seg pass=%d name=%s pitchSet=%.0f..%.0f aimPitch=%.1f..%.1f simplePitch=%.1f..%.1f riseMax=%.1f fallFrames=%d jumpWeightMax=%.2f jumpClips=0x%x"),
		Pass, S.Name, S.PitchFrom, S.PitchTo, SegPitchMin, SegPitchMax, bSimple ? SegSimplePitchMin : 0.f, bSimple ? SegSimplePitchMax : 0.f, SegRiseMax, SegFallFrames, SegJumpWeightMax, SegJumpClips);
}

void UMOBAnimCompareSubsystem::DriveFoe(float DeltaTime)
{
	ACharacter* Target = Foe.Get();
	if (!Target) return;
	const FScriptSegment& S = Seg(Script,Segment);
	BodyYaw += S.YawRate * DeltaTime;
	MoveAngle += S.AngleRate * DeltaTime;

	// 실제 판의 상대처럼 몸 방향과 시선은 바깥에서 정한다(MOBridgeSubsystem::TickRemotes)
	const FRotator Body(0.f, BodyYaw, 0.f);
	Target->SetActorRotation(Body);
	if (!bAimJump)
	{
		if (AController* Ctrl = Target->GetController()) Ctrl->SetControlRotation(Body);
	}
	else
	{
		AimPitch = FMath::Lerp(S.PitchFrom, S.PitchTo, S.Seconds > 0.f ? FMath::Clamp(SegmentTime / S.Seconds, 0.f, 1.f) : 1.f);
		const FRotator Aim(AimPitch, BodyYaw, 0.f);
		if (AController* Ctrl = Target->GetController()) Ctrl->SetControlRotation(Aim);
		if (APawn* Mark = AimMark.Get()) Mark->SetActorLocation(Target->GetPawnViewLocation() + Aim.Vector() * AIM_MARK_DISTANCE);
		if (S.bJump && SegmentTime < JUMP_PRESS_SECONDS) Target->Jump();
		else Target->StopJumping();
	}
	if (S.bMove) Target->AddMovementInput(FRotator(0.f, BodyYaw + MoveAngle, 0.f).Vector(), 1.f);
}

void UMOBAnimCompareSubsystem::UpdateCamera()
{
	const ACharacter* Target = Foe.Get();
	ACameraActor* Cam = Camera.Get();
	if (!Target || !Cam) return;
	const int32 P = FMath::Clamp(Pass, 0, NUM_PASSES - 1);
	const FVector Center = Target->GetActorLocation();
	const FVector Eye = Center + CameraSide * CAMERA_DISTANCE[P] + FVector(0.0, 0.0, CAMERA_HEIGHT[P]);
	Cam->SetActorLocationAndRotation(Eye, (Center - Eye).Rotation());
}

void UMOBAnimCompareSubsystem::WriteRow(float SegTime)
{
	const ACharacter* Target = Foe.Get();
	const USkeletalMeshComponent* Mesh = Target ? Target->GetMesh() : nullptr;
	if (!Mesh) return;
	const FVector L = Target->GetActorLocation();
	const FVector V = Target->GetVelocity();
	const FVector Head = Mesh->GetBoneLocation(TEXT("head"));
	const FVector Pelvis = Mesh->GetBoneLocation(TEXT("pelvis"));
	float Wt[FMOBSimpleAnimProxy::kClips] = {};
	float Tm[FMOBSimpleAnimProxy::kClips] = {};
	if (bSimple) MOBGetSimpleAnimState(Target, Wt, Tm);
	Csv += FString::Printf(TEXT("%d,%d,%s,%.3f,%.2f,%.2f,%.2f,%.1f,%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g"),
		Pass, Frame, Seg(Script,Segment).Name, SegTime, L.X, L.Y, L.Z, V.X, V.Y, Target->GetActorRotation().Yaw, Head.X, Head.Y, Head.Z, Pelvis.X, Pelvis.Y, Pelvis.Z,
		Wt[0], Wt[1], Wt[2], Wt[3], Wt[4], Tm[0], Tm[1], Tm[2], Tm[3], Tm[4]);
	MOBAppendSimplePoseState(bSimple ? Target : nullptr, Csv);	// 자세 계산의 나머지 입력(Shared/AnimPose, 2단계 대조)
	Csv += TEXT("\n");
	if (bShapes) WriteShapes();
}

// 부위별 판정 모양을 몸 기준 좌표로 남긴다 — 프레임을 어긋나게 짝지어 오탈을 계산하는 입력이다(위치·몸 방향은 같다고 친다).
// 줄 형식은 무기 기록의 POSE 줄(UMOBWeaponRecordSubsystem::WriteShapes)과 같다. 둘째 칸만 판*100+경로 조각 번호
void UMOBAnimCompareSubsystem::WriteShapes()
{
	const ACharacter* Target = Foe.Get();
	const USkeletalMeshComponent* Mesh = Target ? Target->GetMesh() : nullptr;
	const UPhysicsAsset* PA = Mesh ? Mesh->GetPhysicsAsset() : nullptr;
	if (!PA) return;

	const FVector Loc = Target->GetActorLocation();
	const FQuat ActorInv = FRotator(0, Target->GetActorRotation().Yaw, 0).Quaternion().Inverse();
	const int32 Part = Pass * 100 + Segment;
	for (const USkeletalBodySetup* B : PA->SkeletalBodySetups)
	{
		if (!B) continue;
		const FBodyInstance* Inst = Mesh->GetBodyInstance(B->BoneName);
		if (!Inst) continue;
		const FTransform BodyW = Inst->GetUnrealWorldTransform();
		// 뼈 자리로도 남긴다(POSEB) — 화면에 그려진 몸은 뼈를 따르고, 언리얼 밖 계산이 내는 것도 뼈 자리다. 크기는 물리 몸체처럼 뺀다
		FTransform BoneW = Mesh->GetBoneTransform(Mesh->GetBoneIndex(B->BoneName));
		BoneW.SetScale3D(FVector::OneVector);
		const int32 Weak = B->GetPhysMaterial() == PhysWeak ? 1 : 0;
		const auto Emit = [&](const TCHAR* Kind, const FTransform& ElemLocal, double A, double C, double D)
		{
			const auto Row = [&](const TCHAR* Head, const FTransform& Base)
			{
				const FTransform W = ElemLocal * Base;
				const FVector P = ActorInv.RotateVector(W.GetLocation() - Loc);
				const FVector X = ActorInv.RotateVector(W.GetUnitAxis(EAxis::X));
				const FVector Y = ActorInv.RotateVector(W.GetUnitAxis(EAxis::Y));
				const FVector Z = ActorInv.RotateVector(W.GetUnitAxis(EAxis::Z));
				ShapeCsv += FString::Printf(TEXT("%s,%d,%d,%s,%s,%d,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f\n"),
					Head, Part, Frame, *B->BoneName.ToString(), Kind, Weak, P.X, P.Y, P.Z, X.X, X.Y, X.Z, Y.X, Y.Y, Y.Z, Z.X, Z.Y, Z.Z, A, C, D);
			};
			Row(TEXT("POSE"), BodyW);
			++ShapeRows;
			Row(TEXT("POSEB"), BoneW);
			++BoneShapeRows;
		};
		for (const FKSphereElem& E : B->AggGeom.SphereElems) Emit(TEXT("sphere"), FTransform(E.Center), E.Radius, 0, 0);
		for (const FKSphylElem& E : B->AggGeom.SphylElems) Emit(TEXT("sphyl"), FTransform(E.Rotation, E.Center), E.Radius, E.Length, 0);
		for (const FKBoxElem& E : B->AggGeom.BoxElems) Emit(TEXT("box"), FTransform(E.Rotation, E.Center), E.X, E.Y, E.Z);
	}
}

// 언리얼 밖 자세 계산의 입력을 뽑는다(candidate1-pilot-plan.md 파일럿 1). 리포가 아니라 Saved/MOB 에 둔다 — Lyra 클립은 Epic 자산이다
//   INFO = 뽑은 간격·몸에 붙은 후처리 애니 / MESH = 메시의 몸 기준 자리(캡슐 중심, 요 0) / BODY = 부위 모양의 뼈 기준 자리와 크기
//   BONE·CLIP·KEY = 뼈대와 클립 자료(MOBDumpSimpleClips)
void UMOBAnimCompareSubsystem::WriteClipDump(int32 SampleHz) const
{
	const ACharacter* Target = Foe.Get();
	const USkeletalMeshComponent* Mesh = Target ? Target->GetMesh() : nullptr;
	const UPhysicsAsset* PA = Mesh ? Mesh->GetPhysicsAsset() : nullptr;
	if (!PA) return;

	const auto Numbers = [](const FTransform& T)
	{
		const FVector P = T.GetTranslation();
		const FQuat Q = T.GetRotation();
		const FVector S = T.GetScale3D();
		return FString::Printf(TEXT("%.6f,%.6f,%.6f,%.9f,%.9f,%.9f,%.9f,%.6f,%.6f,%.6f"), P.X, P.Y, P.Z, Q.X, Q.Y, Q.Z, Q.W, S.X, S.Y, S.Z);
	};

	FString Out;
	Out += FString::Printf(TEXT("INFO,hz,%d,postProcessAnim,%s,mesh,%s,physicsAsset,%s\n"), SampleHz, *GetNameSafe(Mesh->GetPostProcessInstance()), *GetNameSafe(Mesh->GetSkeletalMeshAsset()), *GetNameSafe(PA));
	const FTransform ActorYaw(FRotator(0, Target->GetActorRotation().Yaw, 0), Target->GetActorLocation());
	Out += TEXT("MESH,") + Numbers(Mesh->GetComponentTransform().GetRelativeTransform(ActorYaw)) + TEXT("\n");
	int32 Bodies = 0;
	for (const USkeletalBodySetup* B : PA->SkeletalBodySetups)
	{
		if (!B || !Mesh->GetBodyInstance(B->BoneName)) continue;
		const int32 Weak = B->GetPhysMaterial() == PhysWeak ? 1 : 0;
		const auto Emit = [&](const TCHAR* Kind, const FTransform& ElemLocal, double A, double C, double D)
		{
			const FVector P = ElemLocal.GetTranslation();
			const FQuat Q = ElemLocal.GetRotation();
			Out += FString::Printf(TEXT("BODY,%s,%s,%d,%.6f,%.6f,%.6f,%.9f,%.9f,%.9f,%.9f,%.4f,%.4f,%.4f\n"), *B->BoneName.ToString(), Kind, Weak, P.X, P.Y, P.Z, Q.X, Q.Y, Q.Z, Q.W, A, C, D);
			++Bodies;
		};
		for (const FKSphereElem& E : B->AggGeom.SphereElems) Emit(TEXT("sphere"), FTransform(E.Center), E.Radius, 0, 0);
		for (const FKSphylElem& E : B->AggGeom.SphylElems) Emit(TEXT("sphyl"), FTransform(E.Rotation, E.Center), E.Radius, E.Length, 0);
		for (const FKBoxElem& E : B->AggGeom.BoxElems) Emit(TEXT("box"), FTransform(E.Rotation, E.Center), E.X, E.Y, E.Z);
	}
	const bool bClips = MOBDumpSimpleClips(Target, SampleHz, Out);
	const FString Path = FPaths::ProjectSavedDir() / TEXT("MOB") / FString::Printf(TEXT("animcmp_%s_clips%d.csv"), *Tag, SampleHz);
	const bool bSaved = FFileHelper::SaveStringToFile(Out, *Path);
	UE_LOG(LogMOBAComp, Log, TEXT("MOB_ACOMP clips hz=%d bodies=%d clipsOk=%d chars=%d postProcessAnim=%s saved=%d"), SampleHz, Bodies, bClips ? 1 : 0, Out.Len(), *GetNameSafe(Mesh->GetPostProcessInstance()), bSaved ? 1 : 0);
}

void UMOBAnimCompareSubsystem::TickCapture()
{
	if (!bVideo) return;
	if (!Grabber)
	{
		UGameEngine* GE = Cast<UGameEngine>(GEngine);
		TSharedPtr<FSceneViewport> SV = GE ? GE->SceneViewport : nullptr;
		if (!SV.IsValid())
		{
			UE_LOG(LogMOBAComp, Error, TEXT("MOB_ACOMP_FAIL no scene viewport"));
			bVideo = false;
			return;
		}
		GrabSize = SV->GetSizeXY();
		Grabber = new FFrameGrabber(SV.ToSharedRef(), GrabSize);
		Grabber->StartCapturingFrames();
		RawWriter = IFileManager::Get().CreateFileWriter(*(FPaths::ProjectSavedDir() / TEXT("MOB") / FString::Printf(TEXT("rec_animcmp_%s.bgra"), *Tag)));
		UE_LOG(LogMOBAComp, Log, TEXT("MOB_ACOMP capture_start size=%dx%d"), GrabSize.X, GrabSize.Y);
	}
	if (Frame % CAPTURE_EVERY == 0)
	{
		Grabber->CaptureThisFrame(FFramePayloadPtr());
		++CaptureCalls;
	}
	DrainCapture();
}

void UMOBAnimCompareSubsystem::DrainCapture()
{
	if (!Grabber) return;
	for (FCapturedFrameData& F : Grabber->GetCapturedFrames())
	{
		if (F.BufferSize != GrabSize || !RawWriter) continue;
		RawWriter->Serialize(F.ColorBuffer.GetData(), F.ColorBuffer.Num() * sizeof(FColor));
		++GrabbedFrames;
	}
}

void UMOBAnimCompareSubsystem::Finish()
{
	const FString CsvPath = FPaths::ProjectSavedDir() / TEXT("MOB") / FString::Printf(TEXT("animcmp_%s.csv"), *Tag);
	const bool bSaved = FFileHelper::SaveStringToFile(Csv, *CsvPath);
	UE_LOG(LogMOBAComp, Log, TEXT("MOB_ACOMP done tag=%s frames=%d captureCalls=%d grabbed=%d size=%dx%d csvSaved=%d"), *Tag, Frame, CaptureCalls, GrabbedFrames, GrabSize.X, GrabSize.Y, bSaved ? 1 : 0);
	if (bSimple)
	{
		// 언리얼 밖 상태 계산의 입력(candidate1-impl-plan.md 3단계). 대기 프레임까지 들어 있어 위 csv 와 reset · seq 칸으로 잇는다
		FString Moves = TEXT("reset,seq,dt,vx,vy,vz,ax,ay,az,accel,falling,pitch,ground,hasGap,gap\n");
		FString Body;
		const bool bHasMoves = MOBGetSimpleMoveLog(Foe.Get(), Body);
		Moves += Body;
		const FString MovesPath = FPaths::ProjectSavedDir() / TEXT("MOB") / FString::Printf(TEXT("animcmp_%s_moves.csv"), *Tag);
		const bool bMovesSaved = FFileHelper::SaveStringToFile(Moves, *MovesPath);
		UE_LOG(LogMOBAComp, Log, TEXT("MOB_ACOMP moves has=%d chars=%d saved=%d"), bHasMoves ? 1 : 0, Moves.Len(), bMovesSaved ? 1 : 0);
	}
	if (bShapes)
	{
		const FString ShapePath = FPaths::ProjectSavedDir() / TEXT("MOB") / FString::Printf(TEXT("animcmp_%s_shapes.csv"), *Tag);
		const bool bShapesSaved = FFileHelper::SaveStringToFile(ShapeCsv, *ShapePath);
		UE_LOG(LogMOBAComp, Log, TEXT("MOB_ACOMP shapes rows=%d boneRows=%d weakMat=%d saved=%d"), ShapeRows, BoneShapeRows, PhysWeak ? 1 : 0, bShapesSaved ? 1 : 0);
	}
	if (bProbe)
	{
		const FString ProbePath = FPaths::ProjectSavedDir() / TEXT("MOB") / FString::Printf(TEXT("animcmp_%s_probe.csv"), *Tag);
		const bool bProbeSaved = FFileHelper::SaveStringToFile(ProbeCsv, *ProbePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		UE_LOG(LogMOBAComp, Log, TEXT("MOB_ACOMP probe vars=%d machines=%d chars=%d saved=%d"), ProbeVars.Num(), ProbeMachines.Num(), ProbeCsv.Len(), bProbeSaved ? 1 : 0);
	}
	if (bDumpClips && bSimple)
	{
		// 간격 둘 — 결과가 같으면 간격 탓이 아니고, 다르면 간격을 올려야 한다는 뜻이다(제안값)
		WriteClipDump(60);
		WriteClipDump(120);
	}
	Stage = EStage::Done;
	FPlatformMisc::RequestExit(false, TEXT("MOBAnimCompare"));
}

void UMOBAnimCompareSubsystem::Tick(float DeltaTime)
{
	switch (Stage)
	{
	case EStage::WaitActors:
		WaitSeconds += DeltaTime;
		if (IsMatchPlaying(DeltaTime) && Prepare())
		{
			BeginPass();
			SettleLeft = FIRST_SETTLE_SECONDS;
			Stage = EStage::Settle;
		}
		else if (WaitSeconds > (GetWorld()->GetNetMode() == NM_DedicatedServer ? DEDI_WAIT_LIMIT_SECONDS : WAIT_LIMIT_SECONDS))
		{
			UE_LOG(LogMOBAComp, Error, TEXT("MOB_ACOMP_FAIL no actors after %.0fs"), WaitSeconds);
			Finish();
		}
		break;

	case EStage::Settle:
		ForceLyraIdleBreak(DeltaTime);
		DriveFoe(0.f);
		UpdateCamera();
		DrainCapture();
		SettleLeft -= DeltaTime;
		if (SettleLeft <= 0.f)
		{
			if (Pass == 0) LogBody();
			Stage = EStage::Run;
		}
		break;

	case EStage::Run:
		ForceLyraIdleBreak(DeltaTime);
		DriveFoe(DeltaTime);
		UpdateCamera();
		WriteRow(SegmentTime);
		if (bAimJump) TrackSegment();
		if (bProbe) WriteProbe(SegmentTime);
		TickCapture();
		++Frame;
		SegmentTime += DeltaTime;
		if (SegmentTime >= Seg(Script,Segment).Seconds)
		{
			LogSegment();
			ResetSegmentStats();
			SegmentTime = 0.f;
			++Segment;
			if (Segment >= SegCount(Script))
			{
				++Pass;
				UE_LOG(LogMOBAComp, Log, TEXT("MOB_ACOMP pass_end pass=%d frames=%d"), Pass, Frame);
				if (Pass >= NUM_PASSES)
				{
					Segment = SegCount(Script) - 1;
					Stage = EStage::Stopping;
					if (Grabber) Grabber->StopCapturingFrames();
					break;
				}
				BeginPass();
				SettleLeft = PASS_SETTLE_SECONDS;
				Stage = EStage::Settle;
				break;
			}
			MoveAngle = Seg(Script,Segment).Angle;
		}
		break;

	case EStage::Stopping:
		// 비동기 수집이 다 나올 때까지 기다린다
		DrainCapture();
		if (!Grabber || !Grabber->HasOutstandingFrames())
		{
			if (Grabber)
			{
				Grabber->Shutdown();
				delete Grabber;
				Grabber = nullptr;
			}
			if (RawWriter)
			{
				RawWriter->Close();
				delete RawWriter;
				RawWriter = nullptr;
			}
			Finish();
		}
		break;

	case EStage::Done:
		break;
	}
}
