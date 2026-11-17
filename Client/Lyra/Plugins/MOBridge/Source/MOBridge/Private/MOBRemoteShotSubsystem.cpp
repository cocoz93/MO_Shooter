#include "MOBRemoteShotSubsystem.h"

#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameEngine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FrameGrabber.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Slate/SceneViewport.h"
#include "Weapons/LyraWeaponSpawner.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MOBRemoteShotSubsystem)

DEFINE_LOG_CATEGORY_STATIC(LogMOBRemoteShot, Log, All);

namespace
{
	// 자리 · 카메라는 애니 비교 촬영의 가까운 패스와 같은 값(MOBAnimCompareSubsystem.cpp FIXED_HOME · FIXED_YAW · CAMERA_DISTANCE[0] · CAMERA_HEIGHT[0] · CAMERA_FOV)
	const FVector HOME(-1020.0, -1070.0, 142.0);
	constexpr float START_YAW = 180.f;
	constexpr double CAMERA_DISTANCE = 450.0;
	constexpr double CAMERA_HEIGHT = 40.0;
	constexpr float CAMERA_FOV = 80.f;

	constexpr double GRAB_INTERVAL = 1.0 / 30.0;	// 합격 영상이 30프레임 — 더 찍어도 쓰지 않고 원시 영상만 커진다
	constexpr double HOME_RADIUS = 5.0;				// 이 안이면 시작 자리에 서 있다고 본다
	constexpr double SETTLE_SECONDS = 4.0;			// 몸을 만들고 무기를 쥐고 대기 동작이 자리 잡을 시간(애니 비교 촬영의 FIRST_SETTLE_SECONDS 와 같은 값)
	constexpr double MOVED_RADIUS = 3.0;			// 출발로 보는 거리 — 합성 때는 경로를 맞춰 다시 정한다
	constexpr double SCRIPT_AFTER_MOVE = 2837.0 / 60.0;	// BotClient kStartStopScript 합 2958틱 − 첫 조각(서 있기) 121틱
	constexpr double TAIL_SECONDS = 1.0;
	constexpr double WAIT_LIMIT_SECONDS = 180.0;
	constexpr double CAPTURE_LIMIT_SECONDS = 120.0;

	// 프레임을 요청한 순간의 시각 · 자리 — 그림은 몇 프레임 늦게 돌아오므로 그림에 실어 보낸다
	struct FShotPayload : IFramePayload
	{
		double Seconds = 0;
		FVector At = FVector::ZeroVector;
		float Yaw = 0.f;
	};

	UAbilitySystemComponent* FindASC(AActor* A)
	{
		return A ? UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(A) : nullptr;
	}
}

bool UMOBRemoteShotSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* W = Cast<UWorld>(Outer);
	if (!W || !W->IsGameWorld()) return false;
	FString T;
	return FParse::Value(FCommandLine::Get(), TEXT("MOBRemoteShot="), T);
}

void UMOBRemoteShotSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	FParse::Value(FCommandLine::Get(), TEXT("MOBRemoteShot="), Tag);
	AfterMoveSeconds = SCRIPT_AFTER_MOVE;
	FParse::Value(FCommandLine::Get(), TEXT("MOBRemoteShotAfterMove="), AfterMoveSeconds);	// 경로마다 첫 움직임 뒤 길이가 다르다 — 앉기 경로(D-6)는 record_remote_shot.ps1 이 준다
	GAreScreenMessagesEnabled = false;
	Csv = TEXT("frame,seconds,x,y,z,yaw\n");
}

void UMOBRemoteShotSubsystem::Deinitialize()
{
	if (Stage != EStage::Done && Written > 0)
	{
		// 접속이 먼저 끊겨 게임이 닫혔다 — 찍은 데까지의 시각은 남긴다(없으면 원시 영상을 쓸 수 없다)
		UE_LOG(LogMOBRemoteShot, Error, TEXT("MOB_REMOTESHOT_FAIL closed early stage=%d written=%d"), (int32)Stage, Written);
		SaveCsv();
	}
	ReleaseGrabber();
	Super::Deinitialize();
}

TStatId UMOBRemoteShotSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMOBRemoteShotSubsystem, STATGROUP_Tickables);
}

// 내 캐릭터가 아닌 몸 — 이 촬영에는 상대가 하나뿐이다
ACharacter* UMOBRemoteShotSubsystem::FindTarget() const
{
	UWorld* World = GetWorld();
	const APlayerController* PC = World->GetFirstPlayerController();
	const APawn* Me = PC ? PC->GetPawn() : nullptr;
	if (!Me) return nullptr;
	for (TActorIterator<ACharacter> It(World); It; ++It)
	{
		if (*It != Me && FindASC(*It) && It->GetController()) return *It;
	}
	return nullptr;
}

// 내 캐릭터는 화면에 안 나오게. 충돌을 끄면 바닥을 못 찾아 떨어지므로 움직임부터 끈다
void UMOBRemoteShotSubsystem::HideMe(APawn* Me)
{
	ACharacter* C = Cast<ACharacter>(Me);
	if (!C) return;
	if (USkeletalMeshComponent* MyMesh = C->GetMesh()) MyMesh->SetVisibility(false, true);
	if (UCharacterMovementComponent* MyMove = C->GetCharacterMovement()) MyMove->DisableMovement();
	C->SetActorEnableCollision(false);
	C->SetActorHiddenInGame(true);
	TArray<AActor*> Attached;
	C->GetAttachedActors(Attached, true, true);
	for (AActor* A : Attached)
	{
		if (A) A->SetActorHiddenInGame(true);
	}
}

// 카메라를 몸에 붙인다 — 다리가 몸을 옮기는 틱과 이 틱의 순서가 정해져 있지 않아, 카메라를 따로 옮기면 한 프레임씩 흔들린다.
// 몸 방향은 촬영 내내 180 이라 붙여도 카메라는 돌지 않는다
void UMOBRemoteShotSubsystem::AttachCamera(ACharacter* Found)
{
	ACameraActor* Cam = Camera.Get();
	if (!Cam || !Found) return;
	const FVector Center = Found->GetActorLocation();
	const FVector Side = FRotator(0.f, START_YAW - 90.f, 0.f).Vector();
	const FVector Eye = Center + Side * CAMERA_DISTANCE + FVector(0.0, 0.0, CAMERA_HEIGHT);
	Cam->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	Cam->SetActorLocationAndRotation(Eye, (Center - Eye).Rotation());
	Cam->AttachToActor(Found, FAttachmentTransformRules::KeepWorldTransform);
	Target = Found;
}

bool UMOBRemoteShotSubsystem::Prepare(ACharacter* Found)
{
	UWorld* World = GetWorld();
	APlayerController* PC = World->GetFirstPlayerController();
	APawn* Me = PC ? PC->GetPawn() : nullptr;
	if (!Me) return false;

	for (TActorIterator<ALyraWeaponSpawner> It(World); It; ++It)
	{
		It->SetActorHiddenInGame(true);
		It->SetActorEnableCollision(false);
		It->SetActorTickEnabled(false);
	}
	HideMe(Me);
	MyPawn = Me;

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.ObjectFlags |= RF_Transient;
	ACameraActor* Cam = World->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), FTransform(HOME), SpawnParams);
	if (!Cam)
	{
		UE_LOG(LogMOBRemoteShot, Error, TEXT("MOB_REMOTESHOT_FAIL camera"));
		return false;
	}
	Cam->GetCameraComponent()->SetFieldOfView(CAMERA_FOV);
	Cam->GetCameraComponent()->bConstrainAspectRatio = false;
	Camera = Cam;
	AttachCamera(Found);
	PC->bAutoManageActiveCameraTarget = false;
	PC->SetViewTargetWithBlend(Cam, 0.f);
	if (GEngine && GEngine->GameViewport) GEngine->GameViewport->RemoveAllViewportWidgets();	// 조준점 · 체력바를 뺀다

	UGameEngine* GE = Cast<UGameEngine>(GEngine);
	TSharedPtr<FSceneViewport> SV = GE ? GE->SceneViewport : nullptr;
	if (!SV.IsValid())
	{
		UE_LOG(LogMOBRemoteShot, Error, TEXT("MOB_REMOTESHOT_FAIL no scene viewport"));
		return false;
	}
	GrabSize = SV->GetSizeXY();
	Grabber = new FFrameGrabber(SV.ToSharedRef(), GrabSize);
	Grabber->StartCapturingFrames();
	RawWriter = IFileManager::Get().CreateFileWriter(*(FPaths::ProjectSavedDir() / TEXT("MOB") / FString::Printf(TEXT("rec_remoteshot_%s.bgra"), *Tag)));
	if (!RawWriter)
	{
		UE_LOG(LogMOBRemoteShot, Error, TEXT("MOB_REMOTESHOT_FAIL no writer"));
		return false;
	}
	StartedAt = FPlatformTime::Seconds();
	NextGrabAt = StartedAt;
	UE_LOG(LogMOBRemoteShot, Log, TEXT("MOB_REMOTESHOT ready tag=%s target=%s size=%dx%d"), *Tag, *GetNameSafe(Found), GrabSize.X, GrabSize.Y);
	return true;
}

// 시점을 촬영 카메라에 묶어 둔다 — 경기 시작 때 내 캐릭터가 바뀌면 시점이 새 캐릭터로 넘어간다
void UMOBRemoteShotSubsystem::KeepView()
{
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	ACameraActor* Cam = Camera.Get();
	if (!PC || !Cam) return;
	APawn* Me = PC->GetPawn();
	if (Me != MyPawn.Get())
	{
		HideMe(Me);
		MyPawn = Me;
	}
	if (PC->GetViewTarget() != Cam)
	{
		PC->bAutoManageActiveCameraTarget = false;
		PC->SetViewTargetWithBlend(Cam, 0.f);
		++ViewFixes;
	}
}

void UMOBRemoteShotSubsystem::DrainFrames()
{
	for (FCapturedFrameData& F : Grabber->GetCapturedFrames())
	{
		if (F.BufferSize != GrabSize || !RawWriter) continue;
		RawWriter->Serialize(F.ColorBuffer.GetData(), F.ColorBuffer.Num() * sizeof(FColor));
		const FShotPayload* Info = F.GetPayload<FShotPayload>();
		Csv += FString::Printf(TEXT("%d,%.4f,%.3f,%.3f,%.3f,%.3f\n"), Written, Info->Seconds, Info->At.X, Info->At.Y, Info->At.Z, Info->Yaw);
		++Written;
	}
}

void UMOBRemoteShotSubsystem::ReleaseGrabber()
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
}

void UMOBRemoteShotSubsystem::SaveCsv() const
{
	FFileHelper::SaveStringToFile(Csv, *(FPaths::ProjectSavedDir() / TEXT("MOB") / FString::Printf(TEXT("remoteshot_%s.csv"), *Tag)));
}

void UMOBRemoteShotSubsystem::Finish(bool bOk)
{
	ReleaseGrabber();
	SaveCsv();
	UE_LOG(LogMOBRemoteShot, Log, TEXT("MOB_REMOTESHOT done tag=%s ok=%d requested=%d written=%d viewFixes=%d targetChanges=%d seconds=%.2f"),
		*Tag, bOk ? 1 : 0, Requested, Written, ViewFixes, TargetChanges, StartedAt > 0 ? FPlatformTime::Seconds() - StartedAt : 0.0);
	Stage = EStage::Done;
	FPlatformMisc::RequestExit(false, TEXT("MOBRemoteShot"));
}

void UMOBRemoteShotSubsystem::Tick(float DeltaTime)
{
	switch (Stage)
	{
	case EStage::WaitTarget:
	{
		WaitSeconds += DeltaTime;
		ACharacter* Found = FindTarget();
		if (Found != Target.Get())
		{
			Target = Found;
			HomeSeconds = 0;
		}
		HomeSeconds = Found && FVector::Dist(Found->GetActorLocation(), HOME) < HOME_RADIUS ? HomeSeconds + DeltaTime : 0;
		if (HomeSeconds >= SETTLE_SECONDS)
		{
			if (Prepare(Found)) Stage = EStage::Capture;
			else Finish(false);
		}
		else if (WaitSeconds > WAIT_LIMIT_SECONDS)
		{
			UE_LOG(LogMOBRemoteShot, Error, TEXT("MOB_REMOTESHOT_FAIL target not at home after %.0fs"), WaitSeconds);
			Finish(false);
		}
		break;
	}

	case EStage::Capture:
	{
		KeepView();
		ACharacter* Found = FindTarget();
		if (Found && Found != Target.Get())
		{
			++TargetChanges;
			UE_LOG(LogMOBRemoteShot, Warning, TEXT("MOB_REMOTESHOT target changed new=%s"), *GetNameSafe(Found));
			AttachCamera(Found);
		}
		const double Now = FPlatformTime::Seconds();
		if (Found && Now >= NextGrabAt)
		{
			TSharedPtr<FShotPayload, ESPMode::ThreadSafe> Info = MakeShared<FShotPayload, ESPMode::ThreadSafe>();
			Info->Seconds = Now - StartedAt;
			Info->At = Found->GetActorLocation();
			Info->Yaw = Found->GetActorRotation().Yaw;
			Grabber->CaptureThisFrame(Info);
			++Requested;
			NextGrabAt = FMath::Max(NextGrabAt + GRAB_INTERVAL, Now);	// 프레임이 밀렸으면 몰아 찍지 않는다
		}
		DrainFrames();
		if (Found && MovedAt <= 0 && FVector::Dist2D(Found->GetActorLocation(), HOME) > MOVED_RADIUS)
		{
			MovedAt = Now;
			UE_LOG(LogMOBRemoteShot, Log, TEXT("MOB_REMOTESHOT moved t=%.3f"), Now - StartedAt);
		}
		if (MovedAt > 0 && Now - MovedAt >= AfterMoveSeconds + TAIL_SECONDS)
		{
			Grabber->StopCapturingFrames();
			Stage = EStage::Drain;
		}
		else if (Now - StartedAt > CAPTURE_LIMIT_SECONDS)
		{
			UE_LOG(LogMOBRemoteShot, Error, TEXT("MOB_REMOTESHOT_FAIL no finish moved=%d"), MovedAt > 0 ? 1 : 0);
			Finish(false);
		}
		break;
	}

	case EStage::Drain:
		DrainFrames();
		if (!Grabber->HasOutstandingFrames()) Finish(true);
		break;

	case EStage::Done:
		break;
	}
}
