#include "MOBBodyShotSubsystem.h"

#include "AIController.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "BrainComponent.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "MOBSimpleAnimInstance.h"
#include "DrawDebugHelpers.h"
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

#include UE_INLINE_GENERATED_CPP_BY_NAME(MOBBodyShotSubsystem)

DEFINE_LOG_CATEGORY_STATIC(LogMOBBodyShot, Log, All);

namespace
{
	// 찍을 몸 둘 — Lyra 가 캐릭터마다 무작위로 고르는 두 부품(B_PickRandomCharacter 안에 B_Manny · B_Quinn 과 RandomBool 이 있다)
	const TCHAR* const BODY_PARTS[2] = {
		TEXT("/Game/Characters/Cosmetics/B_Manny.B_Manny_C"),
		TEXT("/Game/Characters/Cosmetics/B_Quinn.B_Quinn_C"),
	};
	const TCHAR* const BODY_NAMES[2] = { TEXT("Manny"), TEXT("Quinn") };

	// 한 장씩. Yaw = 두 몸이 보는 방향. 카메라는 -X 쪽에서 +X 를 본다 → 180 = 카메라를 봄, 90 = 화면 오른쪽을 봄(옆모습)
	struct FShot
	{
		const TCHAR* Name;
		float Yaw;
		bool bShapes;
	};
	const FShot SHOTS[] = {
		{ TEXT("front"),        180.f, false },
		{ TEXT("front_shapes"), 180.f, true  },
		{ TEXT("side"),          90.f, false },
		{ TEXT("side_shapes"),   90.f, true  },
	};
	constexpr int32 NUM_SHOTS = UE_ARRAY_COUNT(SHOTS);

	// 자리는 애니 비교 촬영과 같은 평지(MOBAnimCompareSubsystem 의 FIXED_HOME). 간격 · 거리 · 시야는 두 몸이 한 화면에 들어오게 고른 값이다(제안값)
	const FVector HOME(-1020.0, -1070.0, 142.0);
	constexpr double HALF_SPACING = 85.0;		// 두 몸 사이 170cm — 옆모습에서 총이 옆 몸에 닿지 않는 간격
	constexpr double CAMERA_DISTANCE = 650.0;
	constexpr float CAMERA_FOV = 30.f;			// 좁게 — 원근 때문에 키가 달라 보이지 않게
	constexpr float FIRST_POSE_SECONDS = 4.f;	// 몸을 바꾸고 무기를 쥐고 대기 동작이 자리 잡을 시간(애니 비교 촬영의 FIRST_SETTLE_SECONDS 와 같다)
	constexpr float POSE_SECONDS = 3.f;			// 몸을 돌린 뒤 제자리 돌기 애니가 끝날 시간
	constexpr float MATCH_SETTLE_SECONDS = 3.f;	// 경기 시작 때 캐릭터가 한 번 바뀐다(애니 비교 촬영과 같은 절차)
	constexpr float VIEW_SETTLE_SECONDS = 0.5f;	// 시점을 돌려놓은 뒤 그림이 촬영 카메라로 바뀌어 자리 잡을 시간(제안값)
	constexpr double WAIT_LIMIT_SECONDS = 120.0;
	constexpr int32 GRAB_LIMIT_FRAMES = 120;

	enum class EShape : uint8 { Sphere, Sphyl, Box };

	UAbilitySystemComponent* FindASC(AActor* A)
	{
		return A ? UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(A) : nullptr;
	}

	// 부위 모양 하나하나를 세계 좌표로 넘긴다. A · C · D = 구 반지름 / 캡슐 반지름 · 길이 / 상자 X · Y · Z 전체 길이
	template <typename FnType>
	void ForEachShape(const ACharacter* C, FnType&& Fn)
	{
		const USkeletalMeshComponent* Mesh = C ? C->GetMesh() : nullptr;
		const UPhysicsAsset* PA = Mesh ? Mesh->GetPhysicsAsset() : nullptr;
		if (!PA) return;
		for (const USkeletalBodySetup* B : PA->SkeletalBodySetups)
		{
			if (!B) continue;
			const FBodyInstance* Inst = Mesh->GetBodyInstance(B->BoneName);
			if (!Inst) continue;
			const FTransform BodyW = Inst->GetUnrealWorldTransform();
			for (const FKSphereElem& E : B->AggGeom.SphereElems) Fn(B, EShape::Sphere, FTransform(E.Center) * BodyW, E.Radius, 0.0, 0.0);
			for (const FKSphylElem& E : B->AggGeom.SphylElems) Fn(B, EShape::Sphyl, FTransform(E.Rotation, E.Center) * BodyW, E.Radius, E.Length, 0.0);
			for (const FKBoxElem& E : B->AggGeom.BoxElems) Fn(B, EShape::Box, FTransform(E.Rotation, E.Center) * BodyW, E.X, E.Y, E.Z);
		}
	}

	// 모양의 맨 위 높이(세계 Z)
	double ShapeTop(EShape Kind, const FTransform& W, double A, double C, double D)
	{
		const double Z = W.GetLocation().Z;
		switch (Kind)
		{
		case EShape::Sphere: return Z + A;
		case EShape::Sphyl:  return Z + 0.5 * C * FMath::Abs(W.GetUnitAxis(EAxis::Z).Z) + A;
		case EShape::Box:    return Z + 0.5 * (A * FMath::Abs(W.GetUnitAxis(EAxis::X).Z) + C * FMath::Abs(W.GetUnitAxis(EAxis::Y).Z) + D * FMath::Abs(W.GetUnitAxis(EAxis::Z).Z));
		}
		return Z;
	}
}

bool UMOBBodyShotSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* W = Cast<UWorld>(Outer);
	if (!W || !W->IsGameWorld()) return false;
	FString T;
	return FParse::Value(FCommandLine::Get(), TEXT("MOBBodyShot="), T);
}

void UMOBBodyShotSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	FParse::Value(FCommandLine::Get(), TEXT("MOBBodyShot="), Tag);
	PhysWeak = LoadObject<UPhysicalMaterial>(nullptr, TEXT("/Game/Characters/Heroes/PhysMat_Player_WeakSpot.PhysMat_Player_WeakSpot"));
	GAreScreenMessagesEnabled = false;	// 봇이 그전에 쏠 때 남은 오류 글자가 화면을 가린다
}

void UMOBBodyShotSubsystem::Deinitialize()
{
	if (Grabber)
	{
		Grabber->Shutdown();
		delete Grabber;
		Grabber = nullptr;
	}
	Super::Deinitialize();
}

TStatId UMOBBodyShotSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMOBBodyShotSubsystem, STATGROUP_Tickables);
}

bool UMOBBodyShotSubsystem::IsMatchPlaying(float DeltaTime)
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

// 몸 부품을 바꾼다
bool UMOBBodyShotSubsystem::SetBody(ACharacter* Foe, const TCHAR* PartClassPath) const
{
	return MOBSetBodyPart(Foe, PartClassPath);
}

bool UMOBBodyShotSubsystem::Prepare(bool& bFatal)
{
	UWorld* World = GetWorld();
	APlayerController* PC = World->GetFirstPlayerController();
	ACharacter* Me = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
	if (!Me || !FindASC(Me)) return false;

	TArray<ACharacter*> Found;
	for (TActorIterator<ACharacter> It(World); It; ++It)
	{
		if (*It != Me && FindASC(*It) && It->GetController()) Found.Add(*It);
	}
	if (Found.Num() < 2) return false;
	Found.Sort([](const ACharacter& A, const ACharacter& B) { return A.GetName() < B.GetName(); });

	// 소총으로 바꾼다 — 실제 판의 상대가 쥐는 무기이고, 단순 재생의 클립도 소총 것이다
	UClass* RifleDef = LoadClass<ULyraEquipmentDefinition>(nullptr, TEXT("/ShooterCore/Weapons/Rifle/WID_Rifle.WID_Rifle_C"));
	for (int32 i = 0; i < 2; ++i)
	{
		ACharacter* Foe = Found[i];
		if (AAIController* AI = Cast<AAIController>(Foe->GetController()))
		{
			if (AI->BrainComponent) AI->BrainComponent->StopLogic(TEXT("MOBBodyShot"));
			AI->StopMovement();
			AI->ClearFocus(EAIFocusPriority::Gameplay);
			AI->bSetControlRotationFromPawnOrientation = false;
		}
		ULyraEquipmentManagerComponent* EM = Foe->FindComponentByClass<ULyraEquipmentManagerComponent>();
		// 몸부터 바꾸고 무기를 쥔다. 거꾸로 하면 몸을 바꿀 때 무기 자세 애니가 풀려 팔이 내려간다(첫 촬영에서 봄).
		// 무기는 쥘 때 몸의 꾸밈 표시를 보고 남녀 자세를 고른다
		if (!SetBody(Foe, BODY_PARTS[i]))
		{
			bFatal = true;
			return false;
		}
		if (EM && RifleDef)
		{
			const TArray<ULyraEquipmentInstance*> Old = EM->GetEquipmentInstancesOfType(ULyraEquipmentInstance::StaticClass());
			for (ULyraEquipmentInstance* I : Old) EM->UnequipItem(I);
			EM->EquipItem(RifleDef);
		}
		Foes[i] = Foe;
	}

	// 무기 · 회복 발판을 숨긴다 — 몸을 가린다
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
	ACameraActor* Cam = World->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), FTransform(FRotator::ZeroRotator, HOME - FVector(CAMERA_DISTANCE, 0.0, 0.0)), SpawnParams);
	if (!Cam)
	{
		UE_LOG(LogMOBBodyShot, Error, TEXT("MOB_BODYSHOT_FAIL camera"));
		bFatal = true;
		return false;
	}
	Cam->GetCameraComponent()->SetFieldOfView(CAMERA_FOV);
	Cam->GetCameraComponent()->bConstrainAspectRatio = false;
	Camera = Cam;
	PC->bAutoManageActiveCameraTarget = false;
	PC->SetViewTargetWithBlend(Cam, 0.f);
	if (GEngine && GEngine->GameViewport) GEngine->GameViewport->RemoveAllViewportWidgets();	// 조준점 · 체력바를 뺀다

	UGameEngine* GE = Cast<UGameEngine>(GEngine);
	TSharedPtr<FSceneViewport> SV = GE ? GE->SceneViewport : nullptr;
	if (!SV.IsValid())
	{
		UE_LOG(LogMOBBodyShot, Error, TEXT("MOB_BODYSHOT_FAIL no scene viewport"));
		bFatal = true;
		return false;
	}
	GrabSize = SV->GetSizeXY();
	Grabber = new FFrameGrabber(SV.ToSharedRef(), GrabSize);
	Grabber->StartCapturingFrames();

	UE_LOG(LogMOBBodyShot, Log, TEXT("MOB_BODYSHOT ready tag=%s foes=%s,%s size=%dx%d"), *Tag, *GetNameSafe(Found[0]), *GetNameSafe(Found[1]), GrabSize.X, GrabSize.Y);
	return true;
}

// 내 캐릭터는 화면에 안 나오게. 충돌을 끄면 바닥을 못 찾아 떨어지므로 움직임부터 끈다. 몸에 붙은 무기 · 부품 액터도 같이 숨긴다
void UMOBBodyShotSubsystem::HideMe(APawn* Me)
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

// 시점을 촬영 카메라에 묶어 둔다. 첫 촬영에서 둘째 장부터 시점이 다른 데로 넘어갔다 — 바뀐 까닭은 MOB_BODYSHOT view · pawn 줄로 남긴다.
// 돌려놓았으면 true(그 틱의 그림은 쓰지 않는다)
bool UMOBBodyShotSubsystem::KeepView()
{
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	ACameraActor* Cam = Camera.Get();
	if (!PC || !Cam) return false;
	bool bFixed = false;
	APawn* Me = PC->GetPawn();
	if (Me != MyPawn.Get())
	{
		UE_LOG(LogMOBBodyShot, Warning, TEXT("MOB_BODYSHOT pawn changed old=%s new=%s shot=%d"), *GetNameSafe(MyPawn.Get()), *GetNameSafe(Me), Shot);
		HideMe(Me);
		MyPawn = Me;
		bFixed = true;
	}
	if (PC->GetViewTarget() != Cam)
	{
		if (ViewFixes < 20)
		{
			UE_LOG(LogMOBBodyShot, Warning, TEXT("MOB_BODYSHOT view lost target=%s pawn=%s shot=%d stage=%d"), *GetNameSafe(PC->GetViewTarget()), *GetNameSafe(Me), Shot, (int32)Stage);
		}
		PC->bAutoManageActiveCameraTarget = false;
		PC->SetViewTargetWithBlend(Cam, 0.f);
		++ViewFixes;
		bFixed = true;
	}
	return bFixed;
}

void UMOBBodyShotSubsystem::PlaceFoes(bool bTeleport)
{
	const FRotator Facing(0.f, SHOTS[Shot].Yaw, 0.f);
	for (int32 i = 0; i < 2; ++i)
	{
		ACharacter* Foe = Foes[i].Get();
		if (!Foe) continue;
		if (bTeleport)
		{
			if (UCharacterMovementComponent* Move = Foe->GetCharacterMovement()) Move->StopMovementImmediately();
			Foe->TeleportTo(HOME + FVector(0.0, i == 0 ? -HALF_SPACING : HALF_SPACING, 0.0), Facing, false, true);
		}
		// 실제 판의 상대처럼 몸 방향과 시선은 바깥에서 정한다(MOBridgeSubsystem::TickRemotes)
		Foe->SetActorRotation(Facing);
		if (AController* Ctrl = Foe->GetController()) Ctrl->SetControlRotation(Facing);
	}
}

// 판정 모양을 겹쳐 그린다. 빨강 = 약점(머리 판정), 하늘색 = 그 밖
void UMOBBodyShotSubsystem::DrawShapes() const
{
	const UWorld* World = GetWorld();
	constexpr float Thickness = 0.6f;
	for (int32 i = 0; i < 2; ++i)
	{
		ForEachShape(Foes[i].Get(), [&](const USkeletalBodySetup* B, EShape Kind, const FTransform& W, double A, double C, double D)
		{
			const bool bWeak = PhysWeak && B->GetPhysMaterial() == PhysWeak;
			const FColor Col = bWeak ? FColor(255, 40, 40) : FColor(40, 220, 255);
			const FVector P = W.GetLocation();
			switch (Kind)
			{
			case EShape::Sphere: DrawDebugSphere(World, P, (float)A, 16, Col, false, -1.f, SDPG_Foreground, Thickness); break;
			case EShape::Sphyl:  DrawDebugCapsule(World, P, (float)(0.5 * C + A), (float)A, W.GetRotation(), Col, false, -1.f, SDPG_Foreground, Thickness); break;
			case EShape::Box:    DrawDebugBox(World, P, FVector(A, C, D) * 0.5, W.GetRotation(), Col, false, -1.f, SDPG_Foreground, Thickness); break;
			}
		});
	}
}

// 두 몸의 크기를 숫자로 남긴다. 높이는 전부 발바닥(캡슐 바닥) 기준 cm
void UMOBBodyShotSubsystem::LogBodies() const
{
	for (int32 i = 0; i < 2; ++i)
	{
		const ACharacter* Foe = Foes[i].Get();
		const USkeletalMeshComponent* Mesh = Foe ? Foe->GetMesh() : nullptr;
		if (!Mesh) continue;
		FString Meshes;
		TArray<USkeletalMeshComponent*> Parts;
		Foe->GetComponents<USkeletalMeshComponent>(Parts, true);
		for (const USkeletalMeshComponent* P : Parts) Meshes += GetNameSafe(P ? P->GetSkeletalMeshAsset() : nullptr) + TEXT(" ");
		const double Floor = Foe->GetActorLocation().Z - Foe->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();

		int32 Bodies = 0;
		int32 Weak = 0;
		double Top = -1e9;
		double HeadR = 0.0;
		double HeadLen = 0.0;
		FString WeakBones;
		const USkeletalBodySetup* Last = nullptr;
		ForEachShape(Foe, [&](const USkeletalBodySetup* B, EShape Kind, const FTransform& W, double A, double C, double D)
		{
			++Bodies;
			const bool bWeak = PhysWeak && B->GetPhysMaterial() == PhysWeak;
			if (bWeak && B != Last)
			{
				++Weak;
				WeakBones += B->BoneName.ToString() + TEXT(" ");
			}
			Last = B;
			Top = FMath::Max(Top, ShapeTop(Kind, W, A, C, D));
			if (B->BoneName == TEXT("head"))
			{
				HeadR = A;
				HeadLen = Kind == EShape::Sphyl ? C : 0.0;
			}
		});
		const double HeadBone = Mesh->GetBoneLocation(TEXT("head")).Z - Floor;
		const double Pelvis = Mesh->GetBoneLocation(TEXT("pelvis")).Z - Floor;
		const double Shoulder = FVector::Dist(Mesh->GetBoneLocation(TEXT("upperarm_l")), Mesh->GetBoneLocation(TEXT("upperarm_r")));
		UE_LOG(LogMOBBodyShot, Log, TEXT("MOB_BODYSHOT body i=%d name=%s meshes=[%s] physicsAsset=%s shapes=%d weakBodies=%d weakBones=[%s] headBone=%.1f shapeTop=%.1f headRadius=%.1f headLength=%.1f shoulder=%.1f pelvis=%.1f capsule=%.1f/%.1f"),
			i, BODY_NAMES[i], *Meshes, *GetNameSafe(Mesh->GetPhysicsAsset()), Bodies, Weak, *WeakBones, HeadBone, Top - Floor, HeadR, HeadLen, Shoulder, Pelvis,
			Foe->GetCapsuleComponent()->GetScaledCapsuleRadius(), Foe->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	}
}

bool UMOBBodyShotSubsystem::TakeFrame()
{
	for (FCapturedFrameData& F : Grabber->GetCapturedFrames())
	{
		if (F.BufferSize != GrabSize) continue;
		const FString Path = FPaths::ProjectSavedDir() / TEXT("MOB") / FString::Printf(TEXT("bodyshot_%s_%d_%s.bgra"), *Tag, Shot, SHOTS[Shot].Name);
		const bool bOk = FFileHelper::SaveArrayToFile(TArrayView64<const uint8>(reinterpret_cast<const uint8*>(F.ColorBuffer.GetData()), (int64)F.ColorBuffer.Num() * sizeof(FColor)), *Path);
		// 그 순간 화면을 그린 시점 — 촬영 카메라 자리(HOME - 650, FOV 30)와 다르면 그림을 믿지 않는다
		const APlayerController* PC = GetWorld()->GetFirstPlayerController();
		const APlayerCameraManager* PCM = PC ? PC->PlayerCameraManager.Get() : nullptr;
		const FVector CamLoc = PCM ? PCM->GetCameraLocation() : FVector::ZeroVector;
		UE_LOG(LogMOBBodyShot, Log, TEXT("MOB_BODYSHOT shot n=%d name=%s size=%dx%d saved=%d viewTarget=%s camLoc=(%.0f,%.0f,%.0f) fov=%.1f"), Shot, SHOTS[Shot].Name, GrabSize.X, GrabSize.Y, bOk ? 1 : 0,
			*GetNameSafe(PC ? PC->GetViewTarget() : nullptr), CamLoc.X, CamLoc.Y, CamLoc.Z, PCM ? PCM->GetFOVAngle() : 0.f);
		Saved += bOk ? 1 : 0;
		return true;
	}
	return false;
}

void UMOBBodyShotSubsystem::Finish(bool bOk)
{
	if (Grabber)
	{
		Grabber->StopCapturingFrames();
		Grabber->Shutdown();
		delete Grabber;
		Grabber = nullptr;
	}
	UE_LOG(LogMOBBodyShot, Log, TEXT("MOB_BODYSHOT done tag=%s ok=%d saved=%d/%d viewFixes=%d"), *Tag, bOk ? 1 : 0, Saved, NUM_SHOTS, ViewFixes);
	Stage = EStage::Done;
	FPlatformMisc::RequestExit(false, TEXT("MOBBodyShot"));
}

void UMOBBodyShotSubsystem::Tick(float DeltaTime)
{
	switch (Stage)
	{
	case EStage::WaitActors:
	{
		WaitSeconds += DeltaTime;
		bool bFatal = false;
		if (IsMatchPlaying(DeltaTime) && Prepare(bFatal))
		{
			PlaceFoes(true);
			PoseLeft = FIRST_POSE_SECONDS;
			Stage = EStage::Pose;
		}
		else if (bFatal)
		{
			Finish(false);
		}
		else if (WaitSeconds > WAIT_LIMIT_SECONDS)
		{
			UE_LOG(LogMOBBodyShot, Error, TEXT("MOB_BODYSHOT_FAIL no actors after %.0fs"), WaitSeconds);
			Finish(false);
		}
		break;
	}

	case EStage::Pose:
		PlaceFoes(false);
		if (SHOTS[Shot].bShapes) DrawShapes();
		Grabber->GetCapturedFrames();	// 요청 전에 들어온 것은 버린다
		PoseLeft -= DeltaTime;
		if (KeepView()) PoseLeft = FMath::Max(PoseLeft, VIEW_SETTLE_SECONDS);
		if (PoseLeft <= 0.f)
		{
			if (!bLogged)
			{
				LogBodies();
				bLogged = true;
			}
			Grabber->CaptureThisFrame(FFramePayloadPtr());
			GrabWait = 0;
			Stage = EStage::Grab;
		}
		break;

	case EStage::Grab:
		PlaceFoes(false);
		if (SHOTS[Shot].bShapes) DrawShapes();
		if (KeepView())
		{
			// 찍는 사이에 시점이 풀렸다 — 그 그림은 버리고 다시 기다린다
			Grabber->GetCapturedFrames();
			PoseLeft = VIEW_SETTLE_SECONDS;
			Stage = EStage::Pose;
			break;
		}
		if (TakeFrame())
		{
			++Shot;
			if (Shot >= NUM_SHOTS)
			{
				Finish(Saved == NUM_SHOTS);
				break;
			}
			PlaceFoes(true);
			PoseLeft = POSE_SECONDS;
			Stage = EStage::Pose;
		}
		else if (++GrabWait > GRAB_LIMIT_FRAMES)
		{
			UE_LOG(LogMOBBodyShot, Error, TEXT("MOB_BODYSHOT_FAIL no frame for shot %d"), Shot);
			Finish(false);
		}
		break;

	case EStage::Done:
		break;
	}
}
