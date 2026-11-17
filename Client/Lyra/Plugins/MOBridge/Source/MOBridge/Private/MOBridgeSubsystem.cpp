#include "MOBridgeSubsystem.h"

#include "GameProtocol.h"
#include "MapCollision.h"
#include "MatchRules.h"
#include "Movement.h"
#include "Prediction.h"
#include "Weapon.h"
#include "AnimClips.h"
#include "AnimPose.h"
#include "AnimState.h"

#include <vector>

#include "AIController.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimNode_StateMachine.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/GameEngine.h"
#include "Engine/World.h"
#include "FrameGrabber.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GenericTeamAgentInterface.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "InputCoreTypes.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Slate/SceneViewport.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "UnrealClient.h"

#include "Abilities/GameplayAbilityTargetTypes.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/GameplayMessageSubsystem.h"
#include "MOBServerDamageEffect.h"
#include "MOBSimpleAnimInstance.h"
#include "Physics/LyraCollisionChannels.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"

#include "AbilitySystem/Attributes/LyraHealthSet.h"
#include "AbilitySystem/LyraAbilitySystemComponent.h"
#include "Weapons/LyraGameplayAbility_RangedWeapon.h"
#include "AbilitySystem/Phases/LyraGamePhaseAbility.h"
#include "AbilitySystem/Phases/LyraGamePhaseSubsystem.h"
#include "Character/LyraCharacter.h"
#include "Character/LyraCharacterMovementComponent.h"
#include "Character/LyraHealthComponent.h"
#include "Character/LyraPawnExtensionComponent.h"
#include "Equipment/LyraEquipmentDefinition.h"
#include "Equipment/LyraEquipmentInstance.h"
#include "Equipment/LyraEquipmentManagerComponent.h"
#include "Equipment/LyraQuickBarComponent.h"
#include "GameModes/LyraGameMode.h"
#include "Inventory/LyraInventoryItemDefinition.h"
#include "Inventory/LyraInventoryItemInstance.h"
#include "Inventory/LyraInventoryManagerComponent.h"
#include "LyraGameplayTags.h"
#include "EngineUtils.h"
#include "Weapons/LyraWeaponSpawner.h"
#include "Inventory/LyraInventoryItemDefinition.h"
#include "Components/StaticMeshComponent.h"
#include "Messages/LyraVerbMessage.h"
#include "Player/LyraPlayerState.h"
#include "Teams/LyraTeamSubsystem.h"
#include "Weapons/LyraWeaponStateComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MOBridgeSubsystem)

DEFINE_LOG_CATEGORY_STATIC(LogMOB, Log, All);

namespace
{
	namespace wire = mo::wire;

	constexpr double kInputSeconds = 1.0 / wire::kTickRate;
	constexpr int32 kMaxCatchUpInputs = 8;		// 서버 입력 한도(kMaxInputTokens)와 같다 — 더 밀렸으면 보내도 버려진다
	constexpr double kFocalDistance = 1024.0;	// ULyraGameplayAbility_RangedWeapon::GetTargetingTransform 과 같은 값

	constexpr double kCorrectTolerance = 0.1;	// cm·cm/s — 이 안이면 부동소수 차이로 보고 안 고친다
	constexpr double kVisualSnapDistance = 300.0;	// 이보다 크게 틀어지면 서서히 줄이지 않고 바로 옮긴다
	constexpr float kVisualErrorDecay = 12.f;		// 초당 — 보정 차이가 0.1초면 약 30% 남는다
	constexpr double kRemoteDelayTicks = 6.0;		// 0.1초

	constexpr double kAutoStart = 6.0;
	constexpr double kAutoLeg = 1.2;

	TArray<float> ParseFloats(const FString& S)
	{
		TArray<FString> Parts;
		S.ParseIntoArray(Parts, TEXT(","));
		TArray<float> Out;
		for (const FString& P : Parts) Out.Add(FCString::Atof(*P));
		return Out;
	}

	// Lyra 가 사격 시작점을 정하는 식 그대로 — 카메라 시선 위에서 내 몸에 가장 가까운 점
	// (LyraGameplayAbility_RangedWeapon.cpp GetTargetingTransform, CameraTowardsFocus)
	FVector TargetingStart(APlayerController* PC, const FVector& PawnLocation)
	{
		FVector CamLoc;
		FRotator CamRot;
		PC->GetPlayerViewPoint(CamLoc, CamRot);
		const FVector AimDir = CamRot.Vector().GetSafeNormal();
		const FVector FocalLoc = CamLoc + AimDir * kFocalDistance;
		return FocalLoc + ((PawnLocation - FocalLoc) | AimDir) * AimDir;
	}

	FVector ToFVector(const mo::Vec3& V)
	{
		return FVector(V.x, V.y, V.z);
	}

	// Lyra 능력(다른 모듈)은 이 다리를 모른다 — 함수 포인터로 부른다
	UMOBridgeSubsystem* GBulletBridge = nullptr;

	bool BulletDirectionHook(const FVector& StartTrace, const FVector& AimDir, float SpreadAngle, float SpreadMultiplier, float SpreadExponent, int32 BulletIndex, FVector& OutDir)
	{
		return GBulletBridge && GBulletBridge->ComputeBulletDirection(StartTrace, AimDir, SpreadAngle, SpreadMultiplier, SpreadExponent, BulletIndex, OutDir);
	}

	// 문지기 — 내 사격 능력은 다리가 켤 때만 켜진다. Lyra 입력은 누르고 있는 동안 프레임마다 켜 보는데(WhileInputActive) 그것을 여기서 막는다
	bool AbilityGateHook(const ULyraGameplayAbility_RangedWeapon&, const FGameplayAbilityActorInfo*)
	{
		return !GBulletBridge || GBulletBridge->AllowAbilityActivation();
	}

	// 능력은 한 번 켜지면 FireDelay(블루프린트 타이머) 동안 켜져 있어 한 프레임에 한 발만 쏜다. 화면 프레임이 서버 연사보다 드물면 발이 쌓인다.
	// 셋 = 그림이 세 발 넘게 밀리지 않게 자른 제 판단(넘는 오래된 발은 그리지 않는다)
	constexpr int32 kMaxPendingFires = 3;

	// 위치는 서버(·내 예측) 값을 입히므로 언리얼 이동 계산은 쉰다. 입력 소비와 가속 계산은 그대로 돈다 — 애니가 읽는 값
	void NoLyraMove(ULyraCharacterMovementComponent*, float)
	{
	}

	// 앉음은 다리가 정한다(D-5-c). 이동 컴포넌트는 틱마다 bWantsToCrouch 가 지금 앉음과 다르면 스스로 앉거나 일어서며 캡슐을 옮긴다
	// (UCharacterMovementComponent::UpdateCharacterStateBeforeMovement — PerformMovement 안, 이 갈고리 뒤). Lyra 앉기 키(ALyraCharacter::ToggleCrouch) ·
	// 점프 능력의 일어서기(LyraGameplayAbility_Jump.cpp:55)는 바람만 바꾸므로, 이동 직전에 바람을 지금 앉음으로 되돌리면 Lyra 혼자서는 못 앉는다.
	// 남의 캐릭터도 같은 컴포넌트라 같이 막힌다. 센 수 = Lyra 바람이 지금 앉음과 어긋난 채 이동에 들어간 수(-MOBBreakCrouchBlock 이면 세기만 하고 안 되돌린다)
	int64 GLyraCrouchBlocked = 0;
	bool GCrouchCountOnly = false;

	// Lyra 근접 능력(GA_Melee) 칸 — 입력 표식을 뗀 뒤에는 표식으로 못 찾으므로 손잡이로 기억한다(TickAbilityMelee)
	FGameplayAbilitySpecHandle GMeleeSpecHandle;
	// 예측이 켠 근접을 Lyra 능력이 아직 켜져 있어 못 켤 때 기다리는 프레임 상한. 능력 길이(0.54초, 0-2 cpb1) < 예측 간격(34입력 0.567초)이라
	// 보통 안 기다린다 — 여섯 = 다리 판 약 73프레임/초에서 0.08초(제 판단). 넘으면 그 근접은 Lyra 몽타주 없이 지나간다(MeleeDropped)
	constexpr uint64 kMaxMeleeWaitFrames = 6;
	void KeepBridgeCrouch(ULyraCharacterMovementComponent* Self, FVector&, float)
	{
		const ACharacter* C = Self->GetCharacterOwner();
		if (!C || Self->bWantsToCrouch == C->IsCrouched()) return;
		if (GLyraCrouchBlocked < 20)
		{
			UE_LOG(LogMOB, Log, TEXT("MOB_CROUCH_MISMATCH self=%d want=%d crouched=%d mode=%d frame=%llu revert=%d"),
				C->IsPlayerControlled() ? 1 : 0, Self->bWantsToCrouch ? 1 : 0, C->IsCrouched() ? 1 : 0, (int32)Self->MovementMode.GetValue(), (unsigned long long)GFrameCounter, GCrouchCountOnly ? 0 : 1);
		}
		++GLyraCrouchBlocked;
		if (!GCrouchCountOnly) Self->bWantsToCrouch = C->IsCrouched();
	}

	// 앉음을 서버(·내 예측) 값으로. 앉음 표시를 먼저 바꾼다 — 엔진 눈높이 계산이 이 값을 읽는다(ACharacter::RecalculateBaseEyeHeight).
	// Crouch(true) · UnCrouch(true) = 캡슐 반높이 · 메시 높이 · 눈높이만 바꾸고 자리를 안 옮기며 막힘도 안 보는 쪽이다. 자리는 부르는 쪽이 서버 값으로 입힌다. 바꿨으면 true
	bool ApplyCrouchToCharacter(ACharacter* Ch, bool bCrouched)
	{
		UCharacterMovementComponent* M = Ch->GetCharacterMovement();
		if (!M || Ch->IsCrouched() == bCrouched) return false;
		Ch->SetIsCrouched(bCrouched);
		M->bWantsToCrouch = bCrouched;
		if (bCrouched) M->Crouch(true);
		else M->UnCrouch(true);
		// 엔진이 메시 자리를 직접 고쳐 쓰고 세상 자리는 안 갱신한다(ACharacter::OnStartCrouch 의 GetRelativeLocation_DirectMutable) — 이번 프레임 화면에 들게 갱신한다
		if (USkeletalMeshComponent* Mesh = Ch->GetMesh()) Mesh->UpdateComponentToWorld();
		return true;
	}

	// 메시 높이 = 클래스 기본 메시 높이 + Lift. 기준은 엔진 OnStartCrouch 와 같은 클래스 기본값이고, Lift 는 서버 판정 몸(CRoom::GetPoseAt)과 같은 값을 넘긴다
	void SetMeshLift(ACharacter* Ch, double Lift)
	{
		USkeletalMeshComponent* Mesh = Ch->GetMesh();
		const ACharacter* Default = GetDefault<ACharacter>(Ch->GetClass());
		if (!Mesh || !Default || !Default->GetMesh()) return;
		FVector Rel = Mesh->GetRelativeLocation();
		const double Want = Default->GetMesh()->GetRelativeLocation().Z + Lift;
		if (Rel.Z == Want) return;
		Rel.Z = Want;
		Mesh->SetRelativeLocation(Rel);
	}

	void ApplyMoveToCharacter(ACharacter* Ch, const FVector& Velocity, bool bFalling)
	{
		UCharacterMovementComponent* M = Ch->GetCharacterMovement();
		if (!M) return;
		if (bFalling != M->IsFalling()) M->SetMovementMode(bFalling ? MOVE_Falling : MOVE_Walking);
		M->Velocity = Velocity;
		M->UpdateComponentVelocity();	// 애니가 읽는 GetVelocity 는 이쪽 값이다
	}
}

// S2C_STATE 꼬리에서 한 사람치를 읽는다. 머리 뒤에 겹침 칸 × layerCount, 그 뒤 몽타주 칸 × montageCount(형식은 GameProtocol.h wire::ReadAnimTail 한 벌 — 봇도 같은 것을 부른다).
// 틀린 꼬리면 false 를 돌려주고 At 을 끝으로 옮겨 뒤 사람들도 안 읽는다
static bool ReadAnimTail(const uint8* Data, int32 Size, int32& At, wire::AnimStateWire& Out, int64& BadCount)
{
	if (At >= Size) return false;
	size_t Used = 0;
	if (!wire::ReadAnimTail(Data + At, (size_t)(Size - At), Out, Used))
	{
		++BadCount;
		At = Size;
		return false;
	}
	At += (int32)Used;
	return true;
}

// 남의 캐릭터 한 사람의 받은 애니 상태와 계산해 둔 자세. 칸 번호는 FMOBRemote::Snaps 와 같다
struct FMOBRemoteAnim
{
	wire::AnimStateWire States[FMOBRemote::kSnapshots];
	bool bHas[FMOBRemote::kSnapshots] = {};
	uint8 Weapons[FMOBRemote::kSnapshots] = {};	// 그 칸의 무기 번호(EWeapon) — 상태를 그 무기 묶음으로 푼다(서버 CRoom::GetPoseAt 이 틱 기록의 animWeapon 으로 푸는 것과 같다)
	// 최근 두 틱의 자세 — 틱마다 한 번만 계산하고 프레임마다 섞는다
	uint32 PoseTick[2] = {};
	bool bPoseValid[2] = {};
	std::vector<mo::BoneTransform> PoseLocal[2];
	// 몽타주 알림을 마지막으로 훑은 상태의 틱. false = 처음이거나 죽었다 살아났다(앞 상태를 버리고 보여 줄 상태 하나만 훑는다)
	uint32 NotifyTick = 0;
	bool bNotifyValid = false;
};

// 공용 코드 묶음 — 헤더에 Shared 를 드러내지 않으려고 여기에 둔다
struct FMOBCore
{
	mo::CMapCollision Map;
	mo::MoveParams Params = mo::MakeLyraMoveParams();	// CMoveSim 은 참조로 들고 있다 — 임시 값을 넘기면 안 된다
	mo::CMoveSim Sim;
	mo::MatchRules Rules;
	mo::CSelfPrediction Prediction;

	// 사람끼리 막기(D83) — 내 예측이 볼 남 캡슐. 마지막으로 받은 상태의 산 사람만(서버 · 봇 SetPawnsForPrediction 과 같은 식).
	// 남 자리는 받은 최신 상태라 서버가 내 입력을 굴린 순간의 자리와 다르다 — 그 차이는 Reconcile 보정으로 나온다
	std::vector<mo::PawnCapsule> Pawns;
	// 예측이 걸음을 돌리는 동안만 넘긴다 — Sim 을 쓰는 다른 곳(근접 화면 판정 등)에 남지 않게
	struct FPawnScope
	{
		FMOBCore& C;
		explicit FPawnScope(FMOBCore& InC) : C(InC) { if (C.Rules.pawnBlock) C.Sim.SetPawns(C.Pawns.data(), C.Pawns.size()); }
		~FPawnScope() { C.Sim.SetPawns(nullptr, 0); }
	};

	// 서버 애니(-MOBServerAnim) — 게임 스레드에서만 쓴다
	mo::CAnimClips AnimClips;
	// 무기별 묶음(9단계) — 차례 = 서버 무기 번호(EWeapon), 묶음 이름 = WeaponDef::name. 서버 CRoom 과 같은 짜임
	mo::CAnimState AnimCodecs[(size_t)mo::EWeapon::Count];			// 받은 상태를 자세 입력으로 푸는 데만 쓴다
	mo::CAnimPoseEvaluator AnimEvaluators[(size_t)mo::EWeapon::Count];
	std::vector<mo::BoneTransform> AnimBlend;
	TArray<FName> AnimBoneNames;
	TArray<int32> BundleWeapon;		// 클립 파일 묶음 차례 → 서버 무기 번호(-1 = 무기 묶음 아님). 몽타주 칸의 묶음 번호로 언리얼 경로 자산을 고른다(9단계 5-a)
	TMap<uint32, FMOBRemoteAnim> RemoteAnims;

	FMOBCore()
		: Sim(Map, Params)
		, Prediction(Sim, Rules, Params.maxWalkSpeed)
	{
	}

	bool LoadAnim(const FString& Path, FString& OutWhy)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path))
		{
			OutWhy = TEXT("file");
			return false;
		}
		if (AnimClips.LoadFromMemory(Bytes.GetData(), (size_t)Bytes.Num()) != mo::EClipLoad::Ok)
		{
			OutWhy = TEXT("parse");
			return false;
		}
		for (int32 W = 0; W < (int32)mo::EWeapon::Count; ++W)
		{
			const char* const Bundle = mo::GetWeaponDef((mo::EWeapon)W).name;	// 서버 Room.h AnimBundleOf 와 같은 묶음
			if (!AnimEvaluators[W].Init(AnimClips, Bundle) || !AnimCodecs[W].Init(AnimClips, Bundle))
			{
				OutWhy = FString::Printf(TEXT("bundle=%s"), UTF8_TO_TCHAR(Bundle));
				return false;
			}
		}
		AnimBlend.resize(AnimEvaluators[0].BoneCount());	// 뼈 수는 클립 파일 하나에 하나 — 묶음마다 같다
		for (const mo::ClipBone& Bone : AnimClips.Bones()) AnimBoneNames.Add(FName(UTF8_TO_TCHAR(Bone.name.c_str())));
		BundleWeapon.Init(INDEX_NONE, (int32)AnimClips.Bundles().size());
		for (int32 B = 0; B < BundleWeapon.Num(); ++B)
		{
			for (int32 W = 0; W < (int32)mo::EWeapon::Count; ++W)
			{
				if (AnimClips.Bundles()[(size_t)B].name == mo::GetWeaponDef((mo::EWeapon)W).name) BundleWeapon[B] = W;
			}
		}
		return true;
	}

	// 이 칸의 틱 자세가 든 칸(0·1)을 돌려준다. 없으면 계산해 넣는다. Keep 칸은 덮지 않는다 — 두 틱을 섞을 때 앞 틱 결과를 지키려고
	int32 PoseSlot(FMOBRemoteAnim& Anim, int32 SnapIndex, uint32 Tick, float Pitch, bool bCrouched, int32 Keep, int64& Evals, int64* EvalsByWeapon)
	{
		for (int32 I = 0; I < 2; ++I)
		{
			if (Anim.bPoseValid[I] && Anim.PoseTick[I] == Tick) return I;
		}
		int32 Slot = 0;
		if (Keep != INDEX_NONE) Slot = 1 - Keep;
		else if (!Anim.bPoseValid[0]) Slot = 0;
		else if (!Anim.bPoseValid[1]) Slot = 1;
		else Slot = Anim.PoseTick[0] <= Anim.PoseTick[1] ? 0 : 1;

		const uint8 Weapon = Anim.Weapons[SnapIndex];	// 받을 때 범위를 봤다(AddRemoteSnapshot)
		mo::CAnimPoseEvaluator& Evaluator = AnimEvaluators[Weapon];
		if (Anim.PoseLocal[Slot].size() != Evaluator.BoneCount()) Anim.PoseLocal[Slot].resize(Evaluator.BoneCount());
		mo::AnimPoseInput Input;
		AnimCodecs[Weapon].FromWire(Anim.States[SnapIndex], Pitch, bCrouched, Input);		// 앉음 = 그 칸 사람 상태의 STATE_CROUCHED — 서버가 되감기 기록의 앉음으로 푸는 것과 같다(D62 8)
		Evaluator.EvaluateLocal(Input, Anim.PoseLocal[Slot].data());
		Anim.PoseTick[Slot] = Tick;
		Anim.bPoseValid[Slot] = true;
		++Evals;
		++EvalsByWeapon[Weapon];
		return Slot;
	}
};

// ─────────────────────────────────────────────────────────────
//  켜고 끄기
// ─────────────────────────────────────────────────────────────

bool UMOBridgeSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	if (!World || !(World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE)) return false;
	const TCHAR* Cmd = FCommandLine::Get();
	FString Dummy;
	return FParse::Value(Cmd, TEXT("MOBServer="), Dummy) || FParse::Value(Cmd, TEXT("MOBShotAt="), Dummy) || FParse::Value(Cmd, TEXT("MOBRecord="), Dummy);
}

void UMOBridgeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	ParseCommandLine();
	UE_LOG(LogMOB, Log, TEXT("MOB_INIT world=%s server=%s tag=%s blockDamage=%d autoInput=%d"),
		*GetNameSafe(GetWorld()), *ServerAddr, *Tag, bBlockDamage ? 1 : 0, bAutoInput ? 1 : 0);
}

void UMOBridgeSubsystem::Deinitialize()
{
	if (Socket && !bQuitting) FinishAndQuit(TEXT("closed"));	// 사람이 창을 닫은 경우 — 기록은 남긴다
	if (Socket)
	{
		Socket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
	}
	if (Core)
	{
		ULyraCharacterMovementComponent::MOBMoveHook = nullptr;
		if (ULyraCharacterMovementComponent::MOBPreMoveHook == &KeepBridgeCrouch) ULyraCharacterMovementComponent::MOBPreMoveHook = nullptr;
		ULyraGameplayAbility_RangedWeapon::MOBBulletDirectionHook = nullptr;
		ULyraGameplayAbility_RangedWeapon::MOBCanActivateHook = nullptr;
		GBulletBridge = nullptr;
		delete Core;
		Core = nullptr;
	}
	Super::Deinitialize();
}

TStatId UMOBridgeSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMOBridgeSubsystem, STATGROUP_Tickables);
}

void UMOBridgeSubsystem::ParseCommandLine()
{
	const TCHAR* Cmd = FCommandLine::Get();
	FString S;
	FParse::Value(Cmd, TEXT("MOBServer="), ServerAddr);
	// 목록 값은 쉼표에서 잘리지 않게(FParse::Value 기본값은 쉼표를 구분자로 보고 첫 값만 준다 — 첫 실행에서 캡처가 1장만 찍힌 까닭)
	if (FParse::Value(Cmd, TEXT("MOBShotAt="), S, false)) ShotAt = ParseFloats(S);
	if (FParse::Value(Cmd, TEXT("MOBRecord="), S, false))
	{
		const TArray<float> V = ParseFloats(S);
		if (V.Num() >= 2) { RecStart = V[0]; RecDur = V[1]; }
		if (V.Num() >= 3) RecFps = V[2];
	}
	FParse::Value(Cmd, TEXT("MOBQuitAt="), QuitAt);
	FParse::Value(Cmd, TEXT("MOBMaxFPS="), MaxFpsTest);
	FParse::Value(Cmd, TEXT("MOBTag="), Tag);
	FParse::Value(Cmd, TEXT("MOBBreakPredictAt="), BreakPredictAt);
	FParse::Value(Cmd, TEXT("MOBSelfBodyDelay="), SelfBodyDelay);
	bAutoInput = FParse::Param(Cmd, TEXT("MOBAutoInput"));
	bAutoAim = FParse::Param(Cmd, TEXT("MOBAutoAim"));
	bAutoPitchSweep = FParse::Param(Cmd, TEXT("MOBAutoPitchSweep"));
	FParse::Value(Cmd, TEXT("MOBAutoWeapon="), AutoWeaponFixed);
	bEventShots = FParse::Param(Cmd, TEXT("MOBEventShots"));
	bKeepPickups = FParse::Param(Cmd, TEXT("MOBKeepPickups"));
	FParse::Value(Cmd, TEXT("MOBStandHp="), StandHp);
	FParse::Value(Cmd, TEXT("MOBBreakPadsAt="), BreakPadsAt);
	if (FParse::Value(Cmd, TEXT("MOBStandAt="), S, false))
	{
		const TArray<float> V = ParseFloats(S);
		if (V.Num() == 3)
		{
			bStandOnly = true;
			StandAt = FVector(V[0], V[1], V[2]);
		}
	}
	bServerAnim = FParse::Param(Cmd, TEXT("MOBServerAnim"));
	bSimpleAnim = bServerAnim || FParse::Param(Cmd, TEXT("MOBSimpleAnim"));
	bAnimLog = FParse::Param(Cmd, TEXT("MOBAnimLog"));
	bViewLog = FParse::Param(Cmd, TEXT("MOBViewLog"));
	bRayCheck = FParse::Param(Cmd, TEXT("MOBRayCheck"));
	bAbilityOwnTimer = FParse::Param(Cmd, TEXT("MOBAbilityOwnTimer"));
	bBreakCrouchBlock = FParse::Param(Cmd, TEXT("MOBBreakCrouchBlock"));
	bBreakMeleeBlock = FParse::Param(Cmd, TEXT("MOBBreakMeleeBlock"));
	bAutoMeleeOn = FParse::Param(Cmd, TEXT("MOBAutoMelee"));
	FParse::Value(Cmd, TEXT("MOBRayBreak="), RayBreak);
	bViewClamp = !FParse::Param(Cmd, TEXT("MOBNoViewClamp"));
	FParse::Value(Cmd, TEXT("MOBBreakAt="), BreakAt);
	// 대기 단계는 경험 로드 직후 시작한다 — 준비(캐릭터 생성)를 기다리면 늦는다
	if (!ServerAddr.IsEmpty()) IConsoleManager::Get().FindConsoleVariable(TEXT("Lyra.Bridge.SkipWarmup"))->Set(1, ECVF_SetByCode);
	int32 Block = 1;
	if (FParse::Value(Cmd, TEXT("MOBBlockDamage="), Block)) bBlockDamage = (Block != 0);
}

// 내 캐릭터가 체력까지 붙은 뒤를 「준비」로 본다 — 경험(Experience) 로드가 끝나야 폰이 생긴다
bool UMOBridgeSubsystem::TryBecomeReady()
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	UAbilitySystemComponent* ASC = FindASC(Pawn);
	if (!ASC || !ASC->HasAttributeSetForAttribute(ULyraHealthSet::GetHealthAttribute())) return false;

	ReadyAt = FPlatformTime::Seconds();
	UE_LOG(LogMOB, Log, TEXT("MOB_READY pawn=%s at=(%.1f,%.1f,%.1f)"), *GetNameSafe(Pawn), Pawn->GetActorLocation().X, Pawn->GetActorLocation().Y, Pawn->GetActorLocation().Z);

	if (!ServerAddr.IsEmpty())
	{
		if (bBlockDamage)
		{
			IConsoleManager::Get().FindConsoleVariable(TEXT("Lyra.Bridge.BlockLyraDamage"))->Set(1, ECVF_SetByCode);
			IConsoleManager::Get().FindConsoleVariable(TEXT("Lyra.Bridge.BlockBotRespawn"))->Set(1, ECVF_SetByCode);
			IConsoleManager::Get().FindConsoleVariable(TEXT("Lyra.Bridge.BlockLyraReset"))->Set(1, ECVF_SetByCode);
		}
		Connect();
	}
	return true;
}

// ─────────────────────────────────────────────────────────────
//  매 틱
// ─────────────────────────────────────────────────────────────

void UMOBridgeSubsystem::Tick(float DeltaTime)
{
	if (bQuitting) return;
	if (MaxFpsTest > 0.f && GEngine && GEngine->GetMaxFPS() != MaxFpsTest) GEngine->SetMaxFPS(MaxFpsTest);
	if (ReadyAt < 0 && !TryBecomeReady()) return;

	const double Now = FPlatformTime::Seconds();
	const double T = Now - ReadyAt;
	MaxFrameGap = FMath::Max(MaxFrameGap, (double)DeltaTime);

	PumpSocket();
	if (bQuitting) return;
	if (bAutoInput) TickAutoInput(T);
	TickInput(Now);
	TickAbilityFire();
	TickAbilityMelee();
	if (Core && BreakPredictAt > 0 && T >= BreakPredictAt)
	{
		BreakPredictAt = -1;
		Core->Prediction.DebugNudge(mo::Vec3(100, 0, 0));
		UE_LOG(LogMOB, Log, TEXT("MOB_BREAK_PREDICT t=%.1f seq=%u"), T, Seq);
	}
	if (BreakPadsAt > 0 && T >= BreakPadsAt)
	{
		// 받침(B-3) 일부러 틀림 셋 — 1초 대조가 켜짐 뒤집힘을, 회복 대조가 빠진 회복을, check_pads_bridge.py 가 빠진 소식을 잡아야 한다
		BreakPadsAt = -1;
		bBreakPadHeal = true;
		bBreakPadDrop = true;
		int32 Flipped = -1;
		for (int32 i = 0; i < Pads.Num() && Flipped < 0; ++i)
		{
			if (ALyraWeaponSpawner* A = Pads[i].Actor.Get())
			{
				ApplyPadLook(*A, !Pads[i].bAvailable, false);
				Flipped = i;
			}
		}
		UE_LOG(LogMOB, Log, TEXT("MOB_BREAK_PADS t=%.1f flipped=%d skipHeal=1 dropMsg=1"), T, Flipped);
	}
	if (BreakAt > 0 && T >= BreakAt)
	{
		BreakAt = -1;
		const APlayerController* BreakPC = GetWorld()->GetFirstPlayerController();
		APawn* BreakPawn = BreakPC ? BreakPC->GetPawn() : nullptr;
		UAbilitySystemComponent* BreakASC = FindASC(BreakPawn);
		const ULyraHealthComponent* BreakHC = ULyraHealthComponent::FindHealthComponent(BreakPawn);
		if (BreakASC && BreakHC) BreakASC->SetNumericAttributeBase(ULyraHealthSet::GetHealthAttribute(), FMath::Max(1.f, BreakHC->GetHealth() - 10.f));
		static const FGameplayTag BreakElim = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.Score.Eliminations"));
		if (ULyraTeamSubsystem* BreakTeams = GetWorld()->GetSubsystem<ULyraTeamSubsystem>()) BreakTeams->AddTeamTagStack(LyraTeamOf(Team), BreakElim, 1);
		const ULyraQuickBarComponent* BreakBar = BreakPC ? BreakPC->FindComponentByClass<ULyraQuickBarComponent>() : nullptr;
		if (ULyraInventoryItemInstance* BreakItem = BreakBar ? BreakBar->GetActiveSlotItem() : nullptr)
		{
			BreakItem->AddStatTagStack(FGameplayTag::RequestGameplayTag(TEXT("Lyra.ShooterGame.Weapon.MagazineAmmo")), 7);
		}
		UE_LOG(LogMOB, Log, TEXT("MOB_BREAK t=%.1f hp-10 score+1 mag+7"), T);
		// 도움(A-3) — 내 점수판 도움 +2(Lyra 가 틀림 → 처치 때 맞춤이 고쳐야) · 처치 패킷으로 센 내 도움 +1(센 값 틀림 → 결과 대조가 잡아야)
		static const FGameplayTag BreakAssist = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.Score.Assists"));
		if (ALyraPlayerState* BreakPS = Cast<ALyraPlayerState>(FindPlayerState(PlayerId))) BreakPS->AddStatTagStack(BreakAssist, 2);
		++ServerAssists.FindOrAdd(PlayerId);
		UE_LOG(LogMOB, Log, TEXT("MOB_BREAK_ASSIST id=%u lyra+2 counted+1"), PlayerId);
	}
	TickSelfVisual(Now, DeltaTime);
	TickRemotes(Now);
	TickPickups(Now);
	FlushSend();
	TickCapture(T);

	if (Socket && Now >= NextLogAt)
	{
		NextLogAt = Now + 5.0;
		const APlayerController* LogPC = GetWorld()->GetFirstPlayerController();
		const APawn* LogPawn = LogPC ? LogPC->GetPawn() : nullptr;
		const FVector LyraAt = LogPawn ? LogPawn->GetActorLocation() : FVector::ZeroVector;
		UE_LOG(LogMOB, Log, TEXT("MOB_PROGRESS t=%.1f sent=%lld ack=%u states=%lld hp=%.0f mag=%d spare=%d weapon=%u flags=%u phase=%u score=%u:%u server=(%.0f,%.0f,%.0f) lyra=(%.0f,%.0f,%.0f) pawn=%s compared=%lld corrections=%lld remotes=%d skipped=%lld"),
			T, InputsSent, AckSeq, StatesRecv, ServerHp, ServerMagazine, ServerSpare, ServerWeapon, ServerFlags, MatchPhase, ScoreA, ScoreB, LastServerPosX, LastServerPosY, LastServerPosZ, LyraAt.X, LyraAt.Y, LyraAt.Z, *GetNameSafe(LogPawn), PosDiffCount, Corrections, RemoteSpawned, InputsSkipped);
	}

	if (ResultAt > 0 && bAutoInput && Now - ResultAt > 1.5) FinishAndQuit(TEXT("result"));	// 사람이면 결과 화면을 볼 수 있게 둔다
	else if (QuitAt > 0 && T > QuitAt) FinishAndQuit(TEXT("quit-at"));
}

// ─────────────────────────────────────────────────────────────
//  소켓
// ─────────────────────────────────────────────────────────────

void UMOBridgeSubsystem::Connect()
{
	FString Host = ServerAddr, PortStr = TEXT("7791");
	ServerAddr.Split(TEXT(":"), &Host, &PortStr);

	ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	TSharedRef<FInternetAddr> Addr = SS->CreateInternetAddr();
	bool bValid = false;
	Addr->SetIp(*Host, bValid);
	Addr->SetPort(FCString::Atoi(*PortStr));
	Socket = SS->CreateSocket(NAME_Stream, TEXT("MOBridge"), false);
	if (!bValid || !Socket || !Socket->Connect(*Addr))
	{
		UE_LOG(LogMOB, Error, TEXT("MOB_CONNECT_FAIL addr=%s"), *ServerAddr);
		FinishAndQuit(TEXT("connect-fail"));
		return;
	}
	Socket->SetNonBlocking(true);
	Socket->SetNoDelay(true);

	// 예측은 서버와 같은 맵·같은 코드로 — 맵을 못 읽으면 계속해도 의미가 없다
	const FString MapPath = FPaths::ProjectSavedDir() / TEXT("MOB") / TEXT("map_expanse.mocl");
	TArray<uint8> MapBytes;
	Core = new FMOBCore();
	if (!FFileHelper::LoadFileToArray(MapBytes, *MapPath) || Core->Map.LoadFromMemory(MapBytes.GetData(), (size_t)MapBytes.Num()) != mo::EMapLoad::Ok)
	{
		UE_LOG(LogMOB, Error, TEXT("MOB_MAP_FAIL path=%s bytes=%d"), *MapPath, MapBytes.Num());
		FinishAndQuit(TEXT("map-fail"));
		return;
	}
	ULyraCharacterMovementComponent::MOBMoveHook = &NoLyraMove;
	// 이동 앞 갈고리는 이동 기록(-MOBMoveRecord — Lyra 이동을 켠 채 잰다)도 쓴다. 둘은 같이 켜지 않는다 — 이미 걸려 있으면 덮지 않고 알린다
	GCrouchCountOnly = bBreakCrouchBlock;
	if (bBreakCrouchBlock) UE_LOG(LogMOB, Warning, TEXT("MOB_CROUCH_BLOCK_OFF"));
	if (bBreakMeleeBlock) UE_LOG(LogMOB, Warning, TEXT("MOB_MELEE_BLOCK_OFF"));
	if (!ULyraCharacterMovementComponent::MOBPreMoveHook)
	{
		ULyraCharacterMovementComponent::MOBPreMoveHook = &KeepBridgeCrouch;
	}
	else
	{
		UE_LOG(LogMOB, Error, TEXT("MOB_CROUCH_HOOK_BUSY"));
	}
	if (!bAbilityOwnTimer)
	{
		GBulletBridge = this;
		ULyraGameplayAbility_RangedWeapon::MOBBulletDirectionHook = &BulletDirectionHook;
		ULyraGameplayAbility_RangedWeapon::MOBCanActivateHook = &AbilityGateHook;
	}

	if (bServerAnim)
	{
		const FString ClipPath = FPaths::ProjectSavedDir() / TEXT("MOB") / TEXT("clips.bin");	// 버전 2 = 세 무기 묶음(9단계). 옛 clips_rifle.bin 은 버전 1 이라 못 읽는다
		FString Why;
		if (Core->LoadAnim(ClipPath, Why))
		{
			UE_LOG(LogMOB, Log, TEXT("MOB_SERVERANIM ready bones=%d"), Core->AnimBoneNames.Num());
		}
		else
		{
			UE_LOG(LogMOB, Error, TEXT("MOB_SERVERANIM_FAIL why=%s path=%s"), *Why, *ClipPath);
			bServerAnim = false;
		}
	}

	wire::Hello Hello = wire::Hello();
	Hello.h.size = sizeof(Hello);
	Hello.h.type = (uint16)wire::Msg::C2S_HELLO;
	Hello.protocolVersion = wire::kProtocolVersion;
	QueuePacket(&Hello, sizeof(Hello));
	FlushSend();
	UE_LOG(LogMOB, Log, TEXT("MOB_CONNECTED addr=%s protocol=%u"), *ServerAddr, Hello.protocolVersion);
}

void UMOBridgeSubsystem::QueuePacket(const void* Pkt, int32 Size)
{
	SendBuf.Append(static_cast<const uint8*>(Pkt), Size);
}

void UMOBridgeSubsystem::FlushSend()
{
	if (!Socket || SendBuf.Num() == 0) return;
	int32 Sent = 0;
	Socket->Send(SendBuf.GetData(), SendBuf.Num(), Sent);
	if (Sent > 0) SendBuf.RemoveAt(0, Sent);
	if (SendBuf.Num() > 0) ++SendShort;
}

void UMOBridgeSubsystem::PumpSocket()
{
	if (!Socket) return;
	uint32 PendingBytes = 0;
	while (Socket->HasPendingData(PendingBytes) && PendingBytes > 0)
	{
		const int32 Old = RecvBuf.Num();
		RecvBuf.SetNumUninitialized(Old + (int32)PendingBytes);
		int32 Read = 0;
		Socket->Recv(RecvBuf.GetData() + Old, (int32)PendingBytes, Read);
		RecvBuf.SetNum(Old + FMath::Max(Read, 0));
		if (Read <= 0) break;
	}

	int32 Pos = 0;
	while (RecvBuf.Num() - Pos >= (int32)sizeof(wire::Header))
	{
		wire::Header H;
		FMemory::Memcpy(&H, RecvBuf.GetData() + Pos, sizeof(H));
		if (H.size < sizeof(wire::Header))
		{
			UE_LOG(LogMOB, Error, TEXT("MOB_BAD_HEADER size=%u"), H.size);
			FinishAndQuit(TEXT("bad-header"));
			return;
		}
		if (RecvBuf.Num() - Pos < H.size) break;
		HandlePacket(RecvBuf.GetData() + Pos, H.size);
		Pos += H.size;
	}
	if (Pos > 0) RecvBuf.RemoveAt(0, Pos);

	// 상대가 닫아도 HasPendingData 는 0 바이트라 알려 주지 않고, GetConnectionState 도 Connected 로 남는다 — 들여다보기로 확인한다
	// 받을 것이 없으면 성공(0 바이트), 상대가 닫았거나 끊겼으면 실패다
	uint8 Peek = 0;
	int32 PeekRead = 0;
	const bool bPeerGone = !Socket->Recv(&Peek, 1, PeekRead, ESocketReceiveFlags::Peek);

	// 결과를 받은 뒤의 끊김은 정상이다 — 서버가 방을 닫는 것이고 사람은 결과 화면을 보는 중이다
	if ((bPeerGone || Socket->GetConnectionState() != SCS_Connected) && !bQuitting && ResultAt <= 0)
	{
		FinishAndQuit(TEXT("disconnected"));
	}
}

void UMOBridgeSubsystem::HandlePacket(const uint8* Data, int32 Size)
{
	wire::Header H;
	FMemory::Memcpy(&H, Data, sizeof(H));
	switch ((wire::Msg)H.type)
	{
	case wire::Msg::S2C_WELCOME:
	{
		wire::Welcome M; FMemory::Memcpy(&M, Data, FMath::Min<int32>(Size, sizeof(M)));
		PlayerId = M.playerId;
		Team = M.team;
		bWelcomed = true;
		UE_LOG(LogMOB, Log, TEXT("MOB_WELCOME player=%u team=%u tick=%u debugAllowed=%u"), M.playerId, M.team, M.tick, M.debugAllowed);
		break;
	}
	case wire::Msg::S2C_SPAWN:
	{
		wire::Spawn M; FMemory::Memcpy(&M, Data, FMath::Min<int32>(Size, sizeof(M)));
		++SpawnsRecv;
		if (M.player == PlayerId && bWelcomed)
		{
			bHasSpawn = true;
			Weapon = 0;
			AutoWeapon = 0;
			const FVector At(M.x, M.y, M.z);
			APlayerController* PC = GetWorld()->GetFirstPlayerController();
			APawn* Pawn = PC ? PC->GetPawn() : nullptr;
			const ULyraHealthComponent* HC = ULyraHealthComponent::FindHealthComponent(Pawn);
			if (PC && (!Pawn || (HC && HC->IsDeadOrDying())))
			{
				// 부활은 서버가 정한다 — 쓰러지는 중이어도 지금 새 캐릭터로
				if (ALyraCharacter* Old = Cast<ALyraCharacter>(Pawn)) Old->Reset();
				if (ALyraGameMode* GM = Cast<ALyraGameMode>(GetWorld()->GetAuthGameMode())) GM->RestartPlayer(PC);
				Pawn = PC->GetPawn();
				++SelfRespawns;
				UE_LOG(LogMOB, Log, TEXT("MOB_RESPAWN me pawn=%s"), *GetNameSafe(Pawn));
			}
			if (Pawn)
			{
				const FRotator Rot(0.f, M.yaw, 0.f);
				const bool bMoved = Pawn->TeleportTo(At, Rot);
				bSelfSet = false;
				UE_LOG(LogMOB, Log, TEXT("MOB_TELEPORT moved=%d now=(%.1f,%.1f,%.1f) pawn=%s"), bMoved ? 1 : 0, Pawn->GetActorLocation().X, Pawn->GetActorLocation().Y, Pawn->GetActorLocation().Z, *GetNameSafe(Pawn));
				PC->SetControlRotation(Rot);
			}
			if (Core)
			{
				{
					const FMOBCore::FPawnScope PawnScope(*Core);	// 부활 자리에서 다시 돌리는 걸음도 남 캡슐을 본다(봇과 같다)
					Core->Prediction.OnSpawn(M.x, M.y, M.z, M.lastSeq);
				}
				NextStandAt = 0;	// 촬영 전용 — 시작 자리로 돌아갔으면 바로 다시 옮긴다(맵이 달라 시작 자리는 허공이다)
				bStandPlaced = false;	// 받침 시험(-MOBStandHp) — 새 몸도 다친 채 선다(경기 시작 부활이 체력 100 으로 다시 세웠다, B-3 pb1)
				SelfCur = ToFVector(Core->Prediction.GetMove().location);
				SelfPrev = SelfCur;
				VisualError = FVector::ZeroVector;
			}
			if (Core && Core->Prediction.GetSeq() <= M.lastSeq) MeleeYaw = M.yaw;	// 서버 player.yaw 가 시작 자리 요로 바뀐다(봇과 같다)
			InputsSinceSpawn = 0;
			UE_LOG(LogMOB, Log, TEXT("MOB_SPAWN me tick=%u lastSeq=%u at=(%.1f,%.1f,%.1f) yaw=%.1f start=%u"), M.tick, M.lastSeq, M.x, M.y, M.z, M.yaw, M.startIndex);
		}
		break;
	}
	case wire::Msg::S2C_STATE:
	{
		wire::State M; FMemory::Memcpy(&M, Data, FMath::Min<int32>(Size, sizeof(M)));
		++StatesRecv;
		bool bFoundMe = false;
		const double TickSample = (double)M.tick - FPlatformTime::Seconds() * wire::kTickRate;
		LastStateTick = FMath::Max(LastStateTick, M.tick);
		if (!bHasTickClock || FMath::Abs(TickSample - TickClockOffset) > 30.0)
		{
			TickClockOffset = TickSample;
			bHasTickClock = true;
		}
		else
		{
			TickClockOffset += 0.02 * (TickSample - TickClockOffset);
		}
		int32 TailAt = (int32)offsetof(wire::State, players) + (int32)sizeof(wire::PlayerState) * FMath::Min<int32>(M.count, wire::kMaxPlayersInRoom);	// 꼬리 = 사람 차례대로 애니 상태
		if (Core) Core->Pawns.clear();		// 이 상태에 안 실린 사람(나감)은 빠진다
		int32 MineAt = -1;
		for (int32 i = 0; i < M.count && i < wire::kMaxPlayersInRoom; ++i)
		{
			const wire::PlayerState& P = M.players[i];
			wire::AnimStateWire Anim;
			const bool bAnim = ReadAnimTail(Data, Size, TailAt, Anim, AnimTailBad);
			AnimTailPlayers += bAnim ? 1 : 0;
			if (P.id != PlayerId)
			{
				AddRemoteSnapshot(P, M.tick, bAnim ? &Anim : nullptr);
				if (Core && !(P.flags & wire::STATE_DEAD))
				{
					mo::MoveState Shape;
					Shape.crouched = (P.flags & wire::STATE_CROUCHED) != 0;
					mo::PawnCapsule Capsule;
					Capsule.center = mo::Vec3(P.x, P.y, P.z);
					Capsule.radius = Core->Params.capsuleRadius;
					Capsule.halfHeight = Core->Sim.HalfHeightOf(Shape);
					Capsule.yaw = P.yaw;
					Core->Pawns.push_back(Capsule);
				}
				continue;
			}
			MineAt = i;
		}
		// 내 줄은 남 자리를 이 상태로 다 갱신한 뒤에 고친다 — 다시 돌리는 걸음도 남 캡슐을 본다(봇 OnState 와 같은 차례)
		if (MineAt >= 0)
		{
			const wire::PlayerState& P = M.players[MineAt];
			bFoundMe = true;
			AckSeq = P.lastSeq;
			ServerHp = P.hp;
			ServerMagazine = P.magazine;
			ServerSpare = P.spare;
			ServerFlags = P.flags;
			ServerWeapon = P.weapon;
			LastServerPosX = P.x; LastServerPosY = P.y; LastServerPosZ = P.z;
			MaxAckLag = FMath::Max(MaxAckLag, (int32)(Seq - P.lastSeq));
			ApplySelfState(P);
			SyncSelfWeapon(P);
			if (!(P.flags & wire::STATE_DEAD)) CompareHp(FindPlayerPawn(PlayerId), P.hp);
		}
		if (!bFoundMe) ++StatesWithoutMe;
		break;
	}
	case wire::Msg::S2C_SHOT:
	{
		wire::Shot M; FMemory::Memcpy(&M, Data, FMath::Min<int32>(Size, sizeof(M)));
		HandleShot(M);
		break;
	}
	case wire::Msg::S2C_DAMAGE:
	{
		wire::Damage M; FMemory::Memcpy(&M, Data, FMath::Min<int32>(Size, sizeof(M)));
		++DamagesRecv;
		ApplyServerHp(M.target, M.attacker, M.hpAfter);
		break;
	}
	case wire::Msg::S2C_MATCH:
	{
		wire::Match M; FMemory::Memcpy(&M, Data, FMath::Min<int32>(Size, sizeof(M)));
		ApplyMatch(M);
		break;
	}
	case wire::Msg::S2C_KILL:
	{
		wire::Kill M = wire::Kill(); FMemory::Memcpy(&M, Data, FMath::Min<int32>(Size, sizeof(M)));	// 도움 번호는 수만큼만 온다(A-2) — 뒤는 0
		HandleKill(M);
		break;
	}
	case wire::Msg::S2C_PICKUP:
	{
		wire::Pickup M = wire::Pickup(); FMemory::Memcpy(&M, Data, FMath::Min<int32>(Size, sizeof(M)));
		HandlePickup(M);
		break;
	}
	case wire::Msg::S2C_MELEE:
	{
		wire::Melee M = wire::Melee(); FMemory::Memcpy(&M, Data, FMath::Min<int32>(Size, sizeof(M)));
		HandleMelee(M);
		break;
	}
	case wire::Msg::S2C_RESULT:
	{
		wire::Result M = wire::Result(); FMemory::Memcpy(&M, Data, FMath::Min<int32>(Size, sizeof(M)));	// 사람 칸은 수만큼만 온다 — 뒤는 0
		ResultAt = FPlatformTime::Seconds();
		UE_LOG(LogMOB, Log, TEXT("MOB_RESULT tick=%u score=%u:%u winner=%u players=%u"), M.tick, M.teamScoreA, M.teamScoreB, M.winningTeam, M.count);
		// 도움 대조(A-3) — 결과의 서버 최종값 ↔ 처치 패킷으로 센 값 · Lyra 점수판 값. 다르면 센 값을 서버 값으로 바꾸고 점수판도 맞춘다
		{
			static const FGameplayTag AssistTag = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.Score.Assists"));
			const int32 Count = FMath::Min<int32>(M.count, wire::kMaxPlayersInRoom);
			for (int32 i = 0; i < Count; ++i)
			{
				const wire::PlayerResult& P = M.players[i];
				++AssistResultPlayers;
				int32& Counted = ServerAssists.FindOrAdd(P.id);
				const int32 Before = Counted;
				if (Counted != (int32)P.assists) ++AssistResultCountDiff;
				Counted = P.assists;
				const ALyraPlayerState* LPS = Cast<ALyraPlayerState>(FindPlayerState(P.id));
				const int32 Lyra = LPS ? LPS->GetStatTagStackCount(AssistTag) : -1;
				if (LPS)
				{
					++AssistResultLyraSeen;
					AssistResultLyraSum += Lyra;
					if (Lyra != (int32)P.assists) ++AssistResultLyraDiff;
				}
				if (Before != (int32)P.assists || (LPS && Lyra != (int32)P.assists))
				{
					UE_LOG(LogMOB, Log, TEXT("MOB_ASSIST_DIFF id=%u server=%u counted=%d lyra=%d"), P.id, P.assists, Before, Lyra);
				}
			}
			SyncAssists();
			UE_LOG(LogMOB, Log, TEXT("MOB_ASSIST_RESULT players=%lld countDiff=%lld lyraDiff=%lld lyraSeen=%lld lyraSum=%lld assistsRecv=%lld fixes=%lld"),
				AssistResultPlayers, AssistResultCountDiff, AssistResultLyraDiff, AssistResultLyraSeen, AssistResultLyraSum, AssistsRecv, AssistFixes);
		}
		break;
	}
	default:
		++UnknownPackets;
		UE_LOG(LogMOB, Warning, TEXT("MOB_UNKNOWN type=%u size=%d"), H.type, Size);
	}
}

// ─────────────────────────────────────────────────────────────
//  내 캐릭터 — 입력을 서버와 같은 코드로 미리 돌리고, 서버가 다르다고 하면 그 값에서 다시 돌린다
// ─────────────────────────────────────────────────────────────

void UMOBridgeSubsystem::ApplySelfState(const wire::PlayerState& P)
{
	if (!Core || !bHasSpawn) return;
	// 어긋난 처음 몇 줄 — 어디서부터 다른지 보려고
	static int32 DiffLinesLeft = 40;
	if (const mo::CSelfPrediction::Slot* S = DiffLinesLeft > 0 ? Core->Prediction.FindComparable(P) : nullptr)
	{
		const double DiffNow = FVector::Dist(FVector(P.x, P.y, P.z), FVector((float)S->move.location.x, (float)S->move.location.y, (float)S->move.location.z));
		const double VelNow = FVector::Dist(FVector(P.vx, P.vy, P.vz), FVector((float)S->move.velocity.x, (float)S->move.velocity.y, (float)S->move.velocity.z));
		if (DiffNow > kCorrectTolerance || VelNow > kCorrectTolerance)
		{
			--DiffLinesLeft;
			UE_LOG(LogMOB, Log, TEXT("MOB_DIFF seq=%u cur=%u server=(%.3f,%.3f,%.3f)(%.3f,%.3f,%.3f) f=%u slot=(%.3f,%.3f,%.3f)(%.3f,%.3f,%.3f) m=%d in=(%.3f,%.3f) btn=%u lock=%d"),
				P.lastSeq, Seq, P.x, P.y, P.z, P.vx, P.vy, P.vz, P.flags,
				S->move.location.x, S->move.location.y, S->move.location.z, S->move.velocity.x, S->move.velocity.y, S->move.velocity.z, (int32)S->move.mode,
				S->input.moveX, S->input.moveY, S->input.buttons, S->lockInputs);
		}
	}
	// 조준 — 서버 STATE_ADS ↔ 같은 번호 입력의 내 예측(봇 비교와 같은 식). 켜진 줄 수를 같이 세 비교가 돌았는지 본다
	if (const mo::CSelfPrediction::Slot* S = Core->Prediction.FindComparable(P))
	{
		const bool bServerAds = (P.flags & wire::STATE_ADS) != 0;
		if (bServerAds) ++AdsComparedOn;
		if (bServerAds != S->ads.active) ++AdsFlagDiff;
		// 앉음 — 같은 식(봇 CompareSelf 의 앉음 비교와 같다, D-5)
		const bool bServerCrouch = (P.flags & wire::STATE_CROUCHED) != 0;
		if (bServerCrouch) ++CrouchComparedOn;
		if (bServerCrouch != S->move.crouched) ++CrouchFlagDiff;
	}
	mo::CSelfPrediction::ReconcileResult R;
	{
		const FMOBCore::FPawnScope PawnScope(*Core);
		R = Core->Prediction.Reconcile(P, kCorrectTolerance);
	}
	if (R.compared)
	{
		++PosDiffCount;
		PosDiffSum += R.posDiff;
		PosDiffMax = FMath::Max(PosDiffMax, R.posDiff);
		VelDiffMax = FMath::Max(VelDiffMax, R.velDiff);
		if (R.posDiff > 0) ++PosDiffNonZero;
	}
	if (!R.corrected) return;

	// 계산은 바로 고치고, 화면은 그 차이만큼 남겨 뒀다가 서서히 없앤다
	const FVector After = ToFVector(Core->Prediction.GetMove().location);
	const FVector Delta = SelfCur - After;
	const double Distance = Delta.Size();
	++Corrections;
	CorrectionSum += Distance;
	CorrectionMax = FMath::Max(CorrectionMax, Distance);
	SelfCur = After;
	SelfPrev -= Delta;
	VisualError += Delta;
	if (VisualError.Size() > kVisualSnapDistance)
	{
		VisualError = FVector::ZeroVector;
		++VisualSnaps;
	}
}

void UMOBridgeSubsystem::TickSelfVisual(double Now, float DeltaTime)
{
	if (!Core || !bHasSpawn) return;
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	ACharacter* Ch = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
	if (!Ch) return;
	const ULyraHealthComponent* HC = ULyraHealthComponent::FindHealthComponent(Ch);
	if (HC && HC->IsDeadOrDying()) return;	// 쓰러지는 모습은 Lyra 몫 — 자리를 덮지 않는다
	EnsureSelfBody(Ch);

	const FName PawnName = Ch->GetFName();
	if (!LastPawnName.IsNone() && PawnName != LastPawnName)
	{
		++PawnChanges;
		bSelfSet = false;
		UE_LOG(LogMOB, Warning, TEXT("MOB_PAWN_CHANGED %s -> %s"), *LastPawnName.ToString(), *PawnName.ToString());
	}
	LastPawnName = PawnName;

	// 내가 지난 프레임에 놓은 자리에서 누가 옮겼나 — 0 이어야 Lyra 이동이 꺼진 것이다
	if (bSelfSet)
	{
		const double Drift = FVector::Dist(Ch->GetActorLocation(), SelfLastSet);
		SelfDriftMax = FMath::Max(SelfDriftMax, Drift);
		if (Drift > 1.0) ++SelfDriftOver1cm;
		if (Drift > 1.0 && DriftLinesLeft > 0)
		{
			// 누가 옮겼나 가르기 — Lyra 근접 능력이 켜져 있었나(GA_Melee 의 밀기 · 몽타주) · 상대 몸이 붙어 있었나(겹침 밀어내기)
			--DriftLinesLeft;
			double NearestRemote = -1.0;
			for (const TPair<uint32, FMOBRemote>& Pair : Remotes)
			{
				if (!Pair.Value.Pawn.IsValid()) continue;
				const double D = FVector::Dist2D(Ch->GetActorLocation(), Pair.Value.Pawn->GetActorLocation());
				if (NearestRemote < 0.0 || D < NearestRemote) NearestRemote = D;
			}
			const FVector Moved = Ch->GetActorLocation() - SelfLastSet;
			const UCharacterMovementComponent* CMC = Ch->GetCharacterMovement();
			UE_LOG(LogMOB, Log, TEXT("MOB_SELF_DRIFT frame=%llu seq=%u drift=%.2f moved=(%.2f,%.2f,%.2f) lyraMelee=%d nearestRemote=%.1f mode=%d crouched=%d"),
				(unsigned long long)GFrameCounter, Seq, Drift, Moved.X, Moved.Y, Moved.Z, bLyraMeleeWasActive ? 1 : 0, NearestRemote,
				CMC ? (int32)CMC->MovementMode.GetValue() : -1, Ch->IsCrouched() ? 1 : 0);
		}
	}

	const mo::MoveState& Move = Core->Prediction.GetMove();
	const bool bWaiting = Core->Prediction.IsWaitingSpawn();
	const double Alpha = FMath::Clamp(1.0 - (NextInputAt - Now) / kInputSeconds, 0.0, 1.0);
	VisualError *= FMath::Exp(-kVisualErrorDecay * DeltaTime);
	const FVector Shown = FMath::Lerp(SelfPrev, SelfCur, Alpha) + VisualError;

	// 앉음 = 예측의 지금 값(서버와 같은 코드). 자리는 직전 · 지금 입력 사이를 메우지만 앉음은 섞지 않는다 — 한 입력(1/60초) 안의 차이라 둔다(제 판단).
	// 캡슐 반높이가 Lyra 카메라 중심 높이에 들어가므로(ULyraCameraMode::GetPivotLocation) 자리 입히기 앞에 바꾼다
	if (ApplyCrouchToCharacter(Ch, Move.crouched)) ++SelfCrouchChanges;
	Ch->SetActorLocation(Shown, false, nullptr, ETeleportType::TeleportPhysics);
	SelfLastSet = Ch->GetActorLocation();
	bSelfSet = true;
	ApplyMoveToCharacter(Ch, bWaiting ? FVector::ZeroVector : ToFVector(Move.velocity), !bWaiting && Move.mode == mo::EMoveMode::Falling);
}

// ─────────────────────────────────────────────────────────────
//  남의 캐릭터 — 서버 상태를 0.1초 뒤 시점으로 입힌다. 위치·속도는 서버 값, 애니는 Lyra 그대로
// ─────────────────────────────────────────────────────────────

void UMOBridgeSubsystem::AddRemoteSnapshot(const wire::PlayerState& P, uint32 ServerTick, const wire::AnimStateWire* Anim)
{
	FMOBRemote& R = Remotes.FindOrAdd(P.id);
	R.Id = P.id;
	R.ServerTeam = P.team;
	FMOBSnapshot& S = R.Snaps[R.SnapCount % FMOBRemote::kSnapshots];
	if (bServerAnim)
	{
		FMOBRemoteAnim& Stored = Core->RemoteAnims.FindOrAdd(P.id);
		const int32 Index = (int32)(R.SnapCount % FMOBRemote::kSnapshots);
		const bool bWeaponOk = P.weapon < (uint8)mo::EWeapon::Count;	// 모르는 무기면 풀 묶음이 없다 — 상태 없음으로 둔다
		if (Anim && !bWeaponOk) ++AnimBadWeapon;
		Stored.bHas[Index] = Anim != nullptr && bWeaponOk;
		Stored.Weapons[Index] = bWeaponOk ? P.weapon : 0;
		if (Anim) Stored.States[Index] = *Anim;
	}
	S.Tick = ServerTick;
	S.Pos = FVector(P.x, P.y, P.z);
	S.Vel = FVector(P.vx, P.vy, P.vz);
	S.Yaw = P.yaw;
	S.Pitch = P.pitch;
	S.MoveX = P.moveX;
	S.MoveY = P.moveY;
	S.bFalling = (P.flags & wire::STATE_FALLING) != 0;
	S.bCrouched = (P.flags & wire::STATE_CROUCHED) != 0;
	++R.SnapCount;
	R.Weapon = P.weapon;

	if (P.flags & wire::STATE_DEAD)
	{
		R.bKilled = true;
		R.bRespawnPending = false;
	}
	else if (R.bKilled && !R.bRespawnPending)
	{
		R.bRespawnPending = true;
		R.RespawnTick = ServerTick;
	}
	if (!R.bKilled) CompareHp(R.Pawn.Get(), P.hp);
}

// Lyra 봇 생성 순서를 그대로 밟는다(LyraBotCreationComponent.cpp:100-125). C++ 컨트롤러라 두뇌(행동 트리) 없이 뜬다
void UMOBridgeSubsystem::SpawnRemote(FMOBRemote& R)
{
	UWorld* World = GetWorld();
	UClass* BotClass = LoadClass<AAIController>(nullptr, TEXT("/Script/LyraGame.LyraPlayerBotController"));
	ALyraGameMode* GM = Cast<ALyraGameMode>(World->GetAuthGameMode());
	if (!BotClass || !GM)
	{
		R.bSpawnFailed = true;
		++RemoteSpawnFail;
		UE_LOG(LogMOB, Error, TEXT("MOB_REMOTE_FAIL id=%u botClass=%d gm=%d"), R.Id, BotClass ? 1 : 0, GM ? 1 : 0);
		return;
	}

	if (AAIController* Existing = R.Ctrl.Get())
	{
		// 부활 — 컨트롤러·이름·팀은 그대로, 캐릭터만 새로
		// 죽을 때 Lyra 가 이동 입력을 막는다(LyraCharacter.cpp:354) — Lyra 봇은 ServerRestartController 에서 풀지만 여기는 그 길을 안 탄다.
		// 안 풀면 부활한 캐릭터의 AddMovementInput 이 버려져 가속이 0 이고, 애니가 Idle 인 채 미끄러진다
		Existing->ResetIgnoreInputFlags();
		GM->RestartPlayer(Existing);
		ACharacter* Fresh = Cast<ACharacter>(Existing->GetPawn());
		if (!Fresh)
		{
			R.bSpawnFailed = true;
			++RemoteSpawnFail;
			UE_LOG(LogMOB, Error, TEXT("MOB_REMOTE_FAIL id=%u respawn"), R.Id);
			return;
		}
		if (ULyraPawnExtensionComponent* Ext = ULyraPawnExtensionComponent::FindPawnExtensionComponent(Fresh)) Ext->CheckDefaultInitialization();
		R.Pawn = Fresh;
		if (bSimpleAnim) MOBApplySimpleAnim(Fresh);
		R.ShownWeapon = 0xFF;
		R.Equipped = nullptr;
		++RemoteRespawns;
		return;
	}

	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SP.ObjectFlags |= RF_Transient;
	AAIController* C = World->SpawnActor<AAIController>(BotClass, FVector::ZeroVector, FRotator::ZeroRotator, SP);
	if (!C)
	{
		R.bSpawnFailed = true;
		++RemoteSpawnFail;
		UE_LOG(LogMOB, Error, TEXT("MOB_REMOTE_FAIL id=%u controller"), R.Id);
		return;
	}
	C->bSetControlRotationFromPawnOrientation = false;	// 시선은 서버 값 — 안 끄면 AI 컨트롤러가 몸 방향으로 덮는다
	if (C->PlayerState) C->PlayerState->SetPlayerName(FString::Printf(TEXT("Player%u"), R.Id));

	GM->GenericPlayerInitialization(C);
	GM->RestartPlayer(C);
	ACharacter* Ch = Cast<ACharacter>(C->GetPawn());
	if (!Ch)
	{
		R.bSpawnFailed = true;
		++RemoteSpawnFail;
		UE_LOG(LogMOB, Error, TEXT("MOB_REMOTE_FAIL id=%u pawn"), R.Id);
		return;
	}
	if (ULyraPawnExtensionComponent* Ext = ULyraPawnExtensionComponent::FindPawnExtensionComponent(Ch)) Ext->CheckDefaultInitialization();

	// 색만 맞춘다 — 피해 판정은 서버 몫이라 Lyra 팀은 화면에만 쓰인다
	APlayerController* PC = World->GetFirstPlayerController();
	IGenericTeamAgentInterface* LocalTA = PC ? Cast<IGenericTeamAgentInterface>(PC->PlayerState) : nullptr;
	IGenericTeamAgentInterface* RemoteTA = Cast<IGenericTeamAgentInterface>(C->PlayerState);
	const int32 LocalLyraTeam = LocalTA ? LocalTA->GetGenericTeamId().GetId() : 1;
	const int32 WantLyraTeam = R.ServerTeam == Team ? LocalLyraTeam : (LocalLyraTeam == 1 ? 2 : 1);
	if (RemoteTA) RemoteTA->SetGenericTeamId(FGenericTeamId((uint8)WantLyraTeam));

	R.Ctrl = C;
	R.Pawn = Ch;
	if (bSimpleAnim) MOBApplySimpleAnim(Ch);
	++RemoteSpawned;
	UE_LOG(LogMOB, Log, TEXT("MOB_REMOTE id=%u pawn=%s serverTeam=%u lyraTeam=%d localLyraTeam=%d"), R.Id, *GetNameSafe(Ch), R.ServerTeam, WantLyraTeam, LocalLyraTeam);
}

void UMOBridgeSubsystem::TickRemotes(double Now)
{
	if (!Core || !bHasTickClock || ResultAt > 0) return;	// 경기가 끝나면 상태가 안 온다 — 그 자리에 둔다
	const double WallTick = Now * wire::kTickRate + TickClockOffset - kRemoteDelayTicks;
	double RenderTick = WallTick;
	if (bViewClamp)
	{
		// 상태가 늦게 오면 화면은 마지막 받은 자리에 상대를 세워 둔다. 그때 시계 시점을 그대로 보고하면 서버가 화면보다 앞선 자리로 되감는다.
		// 그래서 보여 줄 시점을 「받은 상태 중 가장 새 것」까지로 묶고, 묶은 시점을 보고한다. 상대마다 받은 틱이 다르면 가장 늦은 쪽에 맞춘다
		double NewestAll = TNumericLimits<double>::Max();
		for (const TPair<uint32, FMOBRemote>& Pair : Remotes)
		{
			const FMOBRemote& R = Pair.Value;
			if (R.bKilled || !R.Pawn.IsValid() || R.SnapCount == 0) continue;
			NewestAll = FMath::Min(NewestAll, (double)R.Snaps[(R.SnapCount - 1) % FMOBRemote::kSnapshots].Tick);
		}
		if (NewestAll < RenderTick)
		{
			RenderTick = NewestAll;
			++ViewClampFrames;
		}
	}
	ShownRenderTick = RenderTick;
	ShownWallTick = WallTick;
	bool bSpawnedThisFrame = false;

	for (TPair<uint32, FMOBRemote>& Pair : Remotes)
	{
		FMOBRemote& R = Pair.Value;
		if (R.bKilled)
		{
			// 쓰러지는 모습은 Lyra 몫이다. 살아난 틱이 보여 줄 시점에 닿으면 새 캐릭터로
			if (!R.bRespawnPending || RenderTick < (double)R.RespawnTick) continue;
			if (ACharacter* Old = R.Pawn.Get())
			{
				if (ALyraCharacter* LyraOld = Cast<ALyraCharacter>(Old)) LyraOld->Reset();
			}
			R.Pawn = nullptr;
			R.bKilled = false;
			R.bRespawnPending = false;
			R.bHasSet = false;
			// 죽어 있던 동안의 상태(서버가 자세를 안 넘긴다)로 알림을 훑지 않게 앞 상태를 버린다
			if (FMOBRemoteAnim* DeadAnim = bServerAnim && Core ? Core->RemoteAnims.Find(R.Id) : nullptr) DeadAnim->bNotifyValid = false;
		}
		if (!R.Pawn.IsValid())
		{
			// 한 프레임에 하나만 — 캐릭터 만들기가 무거워 입력이 밀리지 않게
			if (!R.bSpawnFailed && !bSpawnedThisFrame)
			{
				bSpawnedThisFrame = true;
				SpawnRemote(R);
			}
			if (!R.Pawn.IsValid()) continue;
		}
		if (R.SnapCount == 0) continue;
		ACharacter* Ch = R.Pawn.Get();
		SyncRemoteWeapon(R, Ch);

		// 내가 지난 프레임에 놓은 자리·방향에서 누가 옮겼나
		if (R.bHasSet)
		{
			const double Drift = FVector::Dist(Ch->GetActorLocation(), R.LastSet);
			RemoteDriftMax = FMath::Max(RemoteDriftMax, Drift);
			if (Drift > 1.0) ++RemoteDriftOver1cm;
			RemoteYawDiffMax = FMath::Max(RemoteYawDiffMax, (double)FMath::Abs(FRotator::NormalizeAxis(Ch->GetActorRotation().Yaw - R.LastSetYaw)));
		}

		// 보여 줄 시점을 사이에 둔 두 상태를 찾는다(틱마다 오므로 뒤에서 몇 칸이면 닿는다)
		const FMOBSnapshot& Newest = R.Snaps[(R.SnapCount - 1) % FMOBRemote::kSnapshots];
		R.ShownNewestTick = Newest.Tick;
		const FMOBSnapshot* From = nullptr;
		const FMOBSnapshot* To = nullptr;
		const uint32 Available = FMath::Min<uint32>(R.SnapCount, FMOBRemote::kSnapshots);
		for (uint32 Back = 0; Back < Available; ++Back)
		{
			const FMOBSnapshot& S = R.Snaps[(R.SnapCount - 1 - Back) % FMOBRemote::kSnapshots];
			if ((double)S.Tick <= RenderTick)
			{
				From = &S;
				break;
			}
			To = &S;
		}

		FMOBSnapshot Shown;
		// 서버 애니 — 서버 되감기(CRoom::GetRewoundBody)와 같은 규칙. 바로 다음 틱이 있고 순간이동이 아닐 때만 두 틱을 섞는다
		const FMOBSnapshot* PoseFrom = nullptr;
		const FMOBSnapshot* PoseTo = nullptr;
		float PoseAlpha = 0.f;
		if (From && To)
		{
			const double Span = FMath::Max<double>((double)To->Tick - (double)From->Tick, 1.0);
			const double Alpha = FMath::Clamp((RenderTick - (double)From->Tick) / Span, 0.0, 1.0);
			Shown = *From;
			PoseFrom = From;
			if (FVector::Dist(From->Pos, To->Pos) > wire::kRemoteTeleportDistance)
			{
				++RemoteTeleports;
			}
			else
			{
				Shown.Pos = FMath::Lerp(From->Pos, To->Pos, Alpha);
				Shown.Vel = FMath::Lerp(From->Vel, To->Vel, Alpha);
				Shown.Yaw = From->Yaw + (float)Alpha * FRotator::NormalizeAxis(To->Yaw - From->Yaw);
				Shown.Pitch = FMath::Lerp(From->Pitch, To->Pitch, (float)Alpha);
				if (To->Tick == From->Tick + 1)
				{
					PoseTo = To;
					PoseAlpha = (float)Alpha;
				}
			}
		}
		else if (From)
		{
			Shown = *From;		// 새 상태가 아직 안 왔다 — 그 자리에 둔다
			PoseFrom = From;
			++RemoteUnderrun;
		}
		else
		{
			Shown = *To;		// 쌓인 상태 전부가 보여 줄 시점보다 뒤다(막 나타남)
			PoseFrom = To;
		}
		++RemoteFrames;
		RemoteLagSum += FVector::Dist(Shown.Pos, Newest.Pos);
		RemoteBehindSum += (double)Newest.Tick - RenderTick;

		// 앉음 · 메시 올림 — 서버 판정 몸(CRoom::GetPoseAt)과 같은 규칙. 앞 틱 앉음으로 잡고, 바로 다음 틱과 자세를 섞을 때만 같은 비율로 섞는다(D64).
		// 땅에서 앉은 틱은 중심이 25 내려가고 올림이 25 늘어 섞는 동안에도 메시 세상 높이가 그대로다
		if (ApplyCrouchToCharacter(Ch, PoseFrom->bCrouched)) ++RemoteCrouchChanges;
		const double LiftZ = Core->Sim.CrouchMeshLiftZ();
		double Lift = PoseFrom->bCrouched ? LiftZ : 0.0;
		if (PoseTo) Lift += ((PoseTo->bCrouched ? LiftZ : 0.0) - Lift) * (double)PoseAlpha;
		SetMeshLift(Ch, Lift);
		if (Lift > 0.0) ++RemoteLiftFrames;
		const FRotator Rot(0.f, Shown.Yaw, 0.f);
		Ch->SetActorLocationAndRotation(Shown.Pos, Rot, false, nullptr, ETeleportType::TeleportPhysics);
		R.LastSet = Ch->GetActorLocation();
		R.LastSetYaw = Shown.Yaw;
		R.bHasSet = true;
		ApplyMoveToCharacter(Ch, Shown.Vel, Shown.bFalling);
		if (bViewLog)
		{
			RemotePitchWantMax = FMath::Max(RemotePitchWantMax, (double)FMath::Abs(Shown.Pitch));
			RemotePitchSeenMax = FMath::Max(RemotePitchSeenMax, (double)FMath::Abs(FRotator::NormalizeAxis(Ch->GetBaseAimRotation().Pitch)));
		}
		if (R.Ctrl.IsValid()) R.Ctrl->SetControlRotation(FRotator(Shown.Pitch, Shown.Yaw, 0.f));
		// 위치는 위에서 정했고, 입력은 애니가 읽는 가속을 만들 뿐이다
		if (Shown.MoveX != 0 || Shown.MoveY != 0) Ch->AddMovementInput(FVector(Shown.MoveX / 127.f, Shown.MoveY / 127.f, 0.f), 1.f);
		if (bAnimLog) MeasureRemoteAnim(Ch, R.Id, Shown.Vel, Shown.MoveX, Shown.MoveY, Now);
		if (bServerAnim)
		{
			EnsureRemoteBody(R, Ch);
			ApplyServerAnim(R, Ch, *PoseFrom, PoseTo, PoseAlpha);
		}
	}
}

// 받은 상태로 자세를 만들어 이번 프레임 화면에 넣는다. 서버 판정(CRoom::GetPoseAt)과 같은 차례다 — 틱별 자세를 계산하고 뼈별로 섞는다
void UMOBridgeSubsystem::ApplyServerAnim(FMOBRemote& R, ACharacter* Ch, const FMOBSnapshot& From, const FMOBSnapshot* To, float Alpha)
{
	FMOBRemoteAnim* Anim = Core->RemoteAnims.Find(R.Id);
	const int32 FromIndex = (int32)(&From - R.Snaps);
	if (!Anim || !Anim->bHas[FromIndex])
	{
		++AnimNoState;
		return;
	}
	static_assert(UE_ARRAY_COUNT(AnimPoseEvalsByWeapon) == (size_t)mo::EWeapon::Count, "무기별 자세 계산 수 칸 = 무기 수");
	const uint8 PoseWeapon = Anim->Weapons[FromIndex];	// 앞 틱의 무기 — 섞기와 언리얼 경로(손가락 · 꼬임 뼈) 클립 한 벌을 이 무기로 고른다(서버 GetPoseAt 도 앞 틱 묶음으로 섞는다)
	const int32 FromSlot = Core->PoseSlot(*Anim, FromIndex, From.Tick, From.Pitch, From.bCrouched, INDEX_NONE, AnimPoseEvals, AnimPoseEvalsByWeapon);
	const mo::BoneTransform* Local = Anim->PoseLocal[FromSlot].data();
	const int32 ToIndex = To ? (int32)(To - R.Snaps) : INDEX_NONE;
	if (To && Alpha > 0.f && Anim->bHas[ToIndex])
	{
		const int32 ToSlot = Core->PoseSlot(*Anim, ToIndex, To->Tick, To->Pitch, To->bCrouched, FromSlot, AnimPoseEvals, AnimPoseEvalsByWeapon);
		Core->AnimEvaluators[PoseWeapon].BlendLocal(Anim->PoseLocal[FromSlot].data(), Anim->PoseLocal[ToSlot].data(), Alpha, Core->AnimBlend.data());
		Local = Core->AnimBlend.data();
	}
	mo::AnimPoseInput Input;
	Core->AnimCodecs[PoseWeapon].FromWire(Anim->States[FromIndex], From.Pitch, From.bCrouched, Input);
	// 몽타주 칸마다 무기 번호 — 칸의 묶음은 그 칸을 튼 무기의 것이다(무기를 바꾸는 틱엔 앞 무기 「빼기」와 새 무기 「꺼내기」가 함께 돈다, D51)
	const auto FillMontageWeapons = [this](const mo::AnimPoseInput& In, int32 (&Out)[mo::kAnimMontageMax])
	{
		for (int32 M = 0; M < mo::kAnimMontageMax; ++M)
		{
			const int32 Bundle = M < In.montageCount ? In.montages[M].bundle : INDEX_NONE;
			Out[M] = Core->BundleWeapon.IsValidIndex(Bundle) ? Core->BundleWeapon[Bundle] : INDEX_NONE;
		}
	};
	int32 MontageWeapons[mo::kAnimMontageMax];
	FillMontageWeapons(Input, MontageWeapons);
	FMOBServerAnimResult Result;
	if (MOBSetServerAnim(Ch, Input, PoseWeapon, MontageWeapons, Local, Core->AnimBoneNames.Num(), Core->AnimBoneNames, Result)) ++AnimPoseFrames;
	else ++AnimPoseFail;

	// 몽타주 알림 — 지난 프레임 뒤로 받은 상태를 틱 차례로 다 훑는다. 화면 프레임이 서버 틱보다 드물면(멈칫한 프레임) 그 사이에 생겼다 진 칸이 있고,
	// Lyra 는 긴 프레임에도 지난 구간의 알림을 다 튼다. 처음 · 죽었다 살아난 뒤(bNotifyValid = false)는 보여 줄 상태 하나만 새 칸으로 본다
	const uint32 Available = FMath::Min<uint32>(R.SnapCount, FMOBRemote::kSnapshots);
	for (uint32 Back = Available; Back-- > 0;)
	{
		const int32 Index = (int32)((R.SnapCount - 1 - Back) % FMOBRemote::kSnapshots);
		const FMOBSnapshot& S = R.Snaps[Index];
		if (S.Tick > From.Tick) break;
		if (Anim->bNotifyValid ? S.Tick <= Anim->NotifyTick : S.Tick != From.Tick) continue;
		if (!Anim->bHas[Index]) continue;
		mo::AnimPoseInput StepInput;
		Core->AnimCodecs[Anim->Weapons[Index]].FromWire(Anim->States[Index], S.Pitch, S.bCrouched, StepInput);
		int32 StepWeapons[mo::kAnimMontageMax];
		FillMontageWeapons(StepInput, StepWeapons);
		MOBStepServerMontages(Ch, StepInput, S.Tick, StepWeapons, Anim->bNotifyValid, Result);
		Anim->NotifyTick = S.Tick;
		Anim->bNotifyValid = true;
	}
	if (Result.SetChange > 0) ++AnimSetChanges;
	else if (Result.SetChange < 0) ++AnimSetMisses;
	AnimMontageCellsIn += Input.montageCount;
	AnimMontageCells += Result.MontageCells;
	AnimMontageMisses += Result.MontageMisses;
	AnimAdsLayers += Result.AdsLayers;
	AnimBadLayers += Result.BadLayers;
	AnimCrouchLayers += Result.CrouchLayers;
	AnimStanceLayers += Result.StanceLayers;
	AnimSlotPasses += Result.SlotPasses;
	AnimMaskMissing += Result.bMaskMissing ? 1 : 0;
	AnimWeaponNotifies += Result.WeaponNotifies;
	AnimWeaponPlaying += Result.WeaponPlaying;
	AnimSoundNotifies += Result.SoundNotifies;
	AnimMontageRestarts += Result.MontageRestarts;
	AnimStepStates += Result.StepStates;
	if (Result.MontageCells > 0) ++AnimMontageFrames;
	if (Result.MontageCells > 0 && Result.SlotPasses == 0) ++AnimMontageNoSlot;
}

// 남의 캐릭터 몸을 전부 Quinn 으로 맞춘다(D33). Lyra 가 컨트롤러마다 몸을 무작위로 고르는데, 그 시점이 캐릭터 생성보다 늦을 수 있어 맞을 때까지 다시 건다
void UMOBridgeSubsystem::EnsureRemoteBody(FMOBRemote& R, ACharacter* Ch)
{
	static const FName QUINN_BODY(TEXT("SKM_Quinn_Invis"));
	constexpr int32 BODY_TRY_LIMIT = 20;
	constexpr double BODY_RETRY_SECONDS = 0.5;

	USkeletalMeshComponent* Mesh = Ch->GetMesh();
	const USkeletalMesh* Asset = Mesh ? Mesh->GetSkeletalMeshAsset() : nullptr;
	if (Asset && Asset->GetFName() == QUINN_BODY)
	{
		if (!bAnimBonesChecked)
		{
			// 클립 파일의 뼈대가 이 몸과 같은가 — 부모가 다르면 덮어쓴 자세가 틀어진다
			bAnimBonesChecked = true;
			int32 Found = 0, ParentBad = 0;
			const std::vector<mo::ClipBone>& Bones = Core->AnimClips.Bones();
			for (int32 I = 0; I < Core->AnimBoneNames.Num(); ++I)
			{
				const FName Name = Core->AnimBoneNames[I];
				if (Mesh->GetBoneIndex(Name) == INDEX_NONE) continue;
				++Found;
				const FName Want = Bones[I].parent >= 0 ? Core->AnimBoneNames[Bones[I].parent] : NAME_None;
				ParentBad += Mesh->GetParentBone(Name) != Want ? 1 : 0;
			}
			UE_LOG(LogMOB, Log, TEXT("MOB_SERVERANIM body=%s bones=%d found=%d parentBad=%d tries=%d"), *Asset->GetName(), Core->AnimBoneNames.Num(), Found, ParentBad, R.BodyTries);
		}
		return;
	}
	const double Now = FPlatformTime::Seconds();
	if (Now < R.NextBodyTryAt || R.BodyTries >= BODY_TRY_LIMIT) return;
	R.NextBodyTryAt = Now + BODY_RETRY_SECONDS;
	++R.BodyTries;
	const bool bSet = MOBSetBodyPart(Ch, TEXT("/Game/Characters/Cosmetics/B_Quinn.B_Quinn_C"));
	if (!bSet || R.BodyTries == BODY_TRY_LIMIT) UE_LOG(LogMOB, Error, TEXT("MOB_SERVERANIM_FAIL body id=%u tries=%d set=%d mesh=%s"), R.Id, R.BodyTries, bSet ? 1 : 0, *GetNameSafe(Asset));
}

// 내 몸도 Quinn 으로 맞춘다(D85 — D33 연장). 서버 클립 · 근접 판정 기록이 Quinn · 여성형 무기 층이라, 내 몸이 Manny 면 내 화면 손이 서버 손과
// 권총 12.5cm 다르다(17-d 실측 — 판정은 서버라 결과는 같고 보이는 것만 다르다). 남의 몸(EnsureRemoteBody)과 같은 함수 · 같은 재시도.
// 무기 층은 쥘 때 고르므로 몸만 바꾸면 남성형 층이 남는다(17-d d1q3) — 몸이 Quinn 인데 층이 여성형이 아니면 지금 칸을 다른 칸 → 제 칸으로 다시 쥔다(기록 도구 -MOBCPQuinn 과 같은 길)
void UMOBridgeSubsystem::EnsureSelfBody(ACharacter* Ch)
{
	static const FName QUINN_BODY(TEXT("SKM_Quinn_Invis"));
	constexpr int32 BODY_TRY_LIMIT = 20;
	constexpr int32 REEQUIP_TRY_LIMIT = 5;
	constexpr double BODY_RETRY_SECONDS = 0.5;
	constexpr double REEQUIP_RETRY_SECONDS = 1.0;	// 쥐기 몽타주(권총 약 1초)가 끝날 만큼

	const double Now = FPlatformTime::Seconds();
	if (SelfBodyChecking.Get() != Ch)
	{
		SelfBodyChecking = Ch;
		bSelfBodyDone = false;
		SelfBodyNextTryAt = Now + SelfBodyDelay;	// 시험 -MOBSelfBodyDelay 만 미룬다 — 평소 0
		SelfBodyTries = 0;
		SelfReequipTries = 0;
		SelfEmptyLogs = 0;
		SelfEmptyLogAt = 0;
		SelfEmptySince = 0;
		if (SelfBodyDelay > 0)
		{
			// 시험 — 먼저 Manny 로 바꿔 총을 남성형 층으로 쥐게 하고, 미룬 시간이 지나면 아래가 Quinn 으로 바꾼다(총을 쥔 뒤 몸이 바뀌는 경우).
			// Lyra 가 처음부터 Quinn 을 주면 바꾸기가 안 일어나 시험이 안 된다(첫 판 bodydelay sets=0)
			const bool bManny = MOBSetBodyPart(Ch, TEXT("/Game/Characters/Cosmetics/B_Manny.B_Manny_C"));
			UE_LOG(LogMOB, Log, TEXT("MOB_SELFBODY_TEST_MANNY pawn=%s set=%d"), *GetNameSafe(Ch), bManny ? 1 : 0);
		}
	}
	if (bSelfBodyDone) return;
	if (Now < SelfBodyNextTryAt) return;

	USkeletalMeshComponent* Mesh = Ch->GetMesh();
	const USkeletalMesh* Asset = Mesh ? Mesh->GetSkeletalMeshAsset() : nullptr;
	if (!Asset || Asset->GetFName() != QUINN_BODY)
	{
		if (SelfBodyTries >= BODY_TRY_LIMIT) return;
		if (SelfBodyTries == 0 && SelfBodyDelay > 0 && Mesh)
		{
			// 시험 — 바꾸기 직전 층(총을 이미 쥐었나)을 남긴다
			FString Before;
			const USkeletalMeshComponent* ConstBefore = Mesh;
			for (const UAnimInstance* L : ConstBefore->GetLinkedAnimInstances()) Before += GetNameSafe(L ? L->GetClass() : nullptr) + TEXT(" ");
			UE_LOG(LogMOB, Log, TEXT("MOB_SELFBODY_BEFORE pawn=%s body=%s layers=[%s] delay=%.1f"), *GetNameSafe(Ch), *GetNameSafe(Asset), *Before.TrimEnd(), SelfBodyDelay);
		}
		SelfBodyNextTryAt = Now + BODY_RETRY_SECONDS;
		++SelfBodyTries;
		++SelfBodySets;
		const bool bSet = MOBSetBodyPart(Ch, TEXT("/Game/Characters/Cosmetics/B_Quinn.B_Quinn_C"));
		if (!bSet || SelfBodyTries == BODY_TRY_LIMIT)
		{
			++SelfBodyFails;
			UE_LOG(LogMOB, Error, TEXT("MOB_SELFBODY_FAIL body tries=%d set=%d mesh=%s"), SelfBodyTries, bSet ? 1 : 0, *GetNameSafe(Asset));
		}
		return;
	}

	// 몸은 Quinn — 무기 층을 본다. 층이 아직 없으면(무기를 안 쥠) 쥘 때까지 기다린다 — 그때 고르는 층이 여성형이다
	FString Layers;
	bool bAnyLayer = false, bFeminine = false;
	const USkeletalMeshComponent* ConstMesh = Mesh;		// 층 목록은 const 로만 열려 있다(기록 도구 LogBodies 와 같은 부름)
	for (const UAnimInstance* L : ConstMesh->GetLinkedAnimInstances())
	{
		const FString Name = GetNameSafe(L ? L->GetClass() : nullptr);
		Layers += Name + TEXT(" ");
		bAnyLayer = true;
		bFeminine |= Name.Contains(TEXT("Feminine"));
	}
	if (!bAnyLayer)
	{
		// 층이 빈 채 기다리는 동안을 센다 — 총을 쥔 뒤 몸이 바뀌어 층이 빠진 경우(D88)를 보려고
		++SelfBodyEmptyFrames;
		if (SelfEmptySince == 0) SelfEmptySince = Now;
		const APlayerController* EmptyPC = Cast<APlayerController>(Ch->GetController());
		const ULyraQuickBarComponent* EmptyBar = EmptyPC ? EmptyPC->FindComponentByClass<ULyraQuickBarComponent>() : nullptr;
		const int32 EmptySlot = EmptyBar ? EmptyBar->GetActiveSlotIndex() : -2;
		if (SelfEmptyLogs < 20 && Now >= SelfEmptyLogAt)
		{
			SelfEmptyLogAt = Now + 1.0;
			++SelfEmptyLogs;
			UE_LOG(LogMOB, Log, TEXT("MOB_SELFBODY_EMPTY pawn=%s body=%s activeSlot=%d bodyTries=%d emptyFor=%.2f log=%d"),
				*GetNameSafe(Ch), *Asset->GetName(), EmptySlot, SelfBodyTries, Now - SelfEmptySince, SelfEmptyLogs);
		}
		// 쥔 칸이 없으면(부활 직후 무기 받기 전) 쥘 때 여성형 층이 붙으므로 기다린다. 쥔 칸이 있는데 1초 넘게 비었으면 총을 쥔 뒤 몸이 바뀌어
		// 층이 빠진 것이다 — 다른 무기로 바꿀 때까지 안 돌아온다(bodydelay2 실측 7 · 9초) → 아래 다시 쥐기로 넘긴다(D88)
		if (EmptySlot < 0 || Now - SelfEmptySince < REEQUIP_RETRY_SECONDS) return;
	}
	else
	{
		SelfEmptySince = 0;
	}
	if (bFeminine)
	{
		bSelfBodyDone = true;
		++SelfBodyDonePawns;
		UE_LOG(LogMOB, Log, TEXT("MOB_SELFBODY pawn=%s body=%s layers=[%s] bodyTries=%d reequipTries=%d"), *GetNameSafe(Ch), *Asset->GetName(), *Layers.TrimEnd(), SelfBodyTries, SelfReequipTries);
		return;
	}
	if (SelfReequipTries >= REEQUIP_TRY_LIMIT)
	{
		bSelfBodyDone = true;
		++SelfBodyFails;
		UE_LOG(LogMOB, Error, TEXT("MOB_SELFBODY_FAIL layer tries=%d layers=[%s]"), SelfReequipTries, *Layers.TrimEnd());
		return;
	}
	APlayerController* PC = Cast<APlayerController>(Ch->GetController());
	ULyraQuickBarComponent* Bar = PC ? PC->FindComponentByClass<ULyraQuickBarComponent>() : nullptr;
	if (!Bar) return;
	const int32 Active = Bar->GetActiveSlotIndex();
	const TArray<ULyraInventoryItemInstance*> Slots = Bar->GetSlots();
	int32 Other = INDEX_NONE;
	for (int32 S = 0; S < Slots.Num(); ++S)
	{
		if (S != Active && Slots[S]) { Other = S; break; }
	}
	if (Active == INDEX_NONE || Other == INDEX_NONE) return;
	SelfBodyNextTryAt = Now + REEQUIP_RETRY_SECONDS;
	++SelfReequipTries;
	++SelfReequips;
	Bar->SetActiveSlotIndex(Other);
	Bar->SetActiveSlotIndex(Active);	// 고른 칸은 그대로 — 서버 무기 번호와 같은 칸(SyncSelfWeapon)
	UE_LOG(LogMOB, Log, TEXT("MOB_SELFBODY_REEQUIP slot=%d via=%d layers=[%s] try=%d"), Active, Other, *Layers.TrimEnd(), SelfReequipTries);
}

// 진단 — 안 그려지는 캐릭터는 애니가 안 돌 수 있으므로 그려진 프레임만 상태를 센다. 내 캐릭터와 나란히 찍어 대조한다
void UMOBridgeSubsystem::MeasureRemoteAnim(ACharacter* Ch, uint32 Id, const FVector& Vel, int8 MoveX, int8 MoveY, double Now)
{
	USkeletalMeshComponent* Mesh = Ch->GetMesh();
	UAnimInstance* Anim = Mesh ? Mesh->GetAnimInstance() : nullptr;
	const UCharacterMovementComponent* Move = Ch->GetCharacterMovement();
	if (!Anim || !Move) return;

	const auto StateOf = [](UAnimInstance* AI)
	{
		const FAnimNode_StateMachine* SM = AI ? AI->GetStateMachineInstanceFromName(TEXT("LocomotionSM")) : nullptr;
		return SM ? SM->GetCurrentStateName() : FName(TEXT("NoMachine"));
	};
	const FName State = StateOf(Anim);
	const bool bRendered = Mesh->WasRecentlyRendered(0.2f);
	if (Vel.Size2D() > 100.0)
	{
		++AnimMoving;
		if (MoveX == 0 && MoveY == 0) ++AnimMovingNoInput;
		if (Move->GetCurrentAcceleration().IsNearlyZero()) ++AnimMovingNoAccel;
		if (bRendered)
		{
			++AnimMovingRendered;
			AnimStates.FindOrAdd(State)++;
		}
	}

	if (!bRendered || Now < NextAnimLogAt) return;
	NextAnimLogAt = Now + 0.5;
	FString Linked;
	Mesh->ForEachAnimInstance([&Linked, Anim](UAnimInstance* L) { if (L != Anim) Linked += GetNameSafe(L->GetClass()) + TEXT(" "); });
	FString SelfText = TEXT("none");
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		if (ACharacter* Me = Cast<ACharacter>(PC->GetPawn()))
		{
			UAnimInstance* MyAnim = Me->GetMesh() ? Me->GetMesh()->GetAnimInstance() : nullptr;
			FString MyLinked;
			if (MyAnim) Me->GetMesh()->ForEachAnimInstance([&MyLinked, MyAnim](UAnimInstance* L) { if (L != MyAnim) MyLinked += GetNameSafe(L->GetClass()) + TEXT(" "); });
			SelfText = FString::Printf(TEXT("state=%s speed=%.0f accel=%.0f mode=%d linked=[%s]"), *StateOf(MyAnim).ToString(), Me->GetVelocity().Size2D(),
				Me->GetCharacterMovement()->GetCurrentAcceleration().Size(), (int32)Me->GetCharacterMovement()->MovementMode.GetValue(), *MyLinked);
		}
	}
	UE_LOG(LogMOB, Log, TEXT("MOB_ANIM id=%u state=%s speed=%.0f compVel=%.0f accel=%.0f move=%d,%d mode=%d tick=%d anim=%s linked=[%s] | self %s"), Id, *State.ToString(), Vel.Size2D(), Ch->GetVelocity().Size2D(),
		Move->GetCurrentAcceleration().Size(), MoveX, MoveY, (int32)Move->MovementMode.GetValue(), (int32)Mesh->VisibilityBasedAnimTickOption, *GetNameSafe(Anim->GetClass()), *Linked, *SelfText);
}

// ─────────────────────────────────────────────────────────────
//  입력 — 프레임과 상관없이 초당 60개. 서버는 틱마다 하나씩 꺼낸다
// ─────────────────────────────────────────────────────────────

void UMOBridgeSubsystem::TickInput(double Now)
{
	if (!Socket || !Core || !bHasSpawn || ResultAt > 0) return;
	if (bStandOnly)
	{
		// 서버 코어는 60초 동안 아무것도 안 받은 접속을 끊는다(IOCPServer.h SESSION_TIMEOUT_SEC) — 그 3분의 1 간격으로 알린다
		constexpr double StandIntervalSeconds = 20.0;
		if (Now < NextStandAt) return;
		NextStandAt = Now + StandIntervalSeconds;
		wire::DebugPlace Place = wire::DebugPlace();
		Place.h.size = sizeof(Place);
		Place.h.type = (uint16)wire::Msg::C2S_DEBUG_PLACE;
		Place.seq = ++Seq;
		Place.x = StandAt.X;
		Place.y = StandAt.Y;
		Place.z = StandAt.Z;
		// 받침 시험(-MOBStandHp)은 부활 뒤 첫 자리 옮김만 그 체력 — 뒤 알림까지 그 값이면 회복된 체력을 조용히 되돌린다
		Place.hp = StandHp <= 0.f ? 100.f : (!bStandPlaced || ServerHp <= 0.f ? StandHp : ServerHp);
		bStandPlaced = true;
		QueuePacket(&Place, sizeof(Place));
		return;
	}
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	ACharacter* Ch = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
	if (!Ch)
	{
		NextInputAt = 0;	// 캐릭터가 없는 동안 쌓인 몫을 몰아 보내지 않는다
		return;
	}

	if (NextInputAt <= 0) NextInputAt = Now;
	int32 Due = (int32)((Now - NextInputAt) / kInputSeconds) + (Now >= NextInputAt ? 1 : 0);
	if (Due <= 0) return;
	if (Due > kMaxCatchUpInputs)
	{
		InputsSkipped += Due - kMaxCatchUpInputs;
		Due = kMaxCatchUpInputs;
		NextInputAt = Now;
	}
	else
	{
		NextInputAt += Due * kInputSeconds;
	}

	// Lyra 입력은 AddMovementInput 을 앞·옆 따로 불러 대각선이면 길이가 1 을 넘는다. 서버 입력은 길이 1 이하가 약속이다
	FVector Move = Ch->GetLastMovementInputVector();
	Move.Z = 0;
	InputRawMax = FMath::Max(InputRawMax, Move.Size());
	if (FMath::Abs(Move.X) > 0.01 && FMath::Abs(Move.Y) > 0.01) ++InputsDiagonal;
	if (Move.SizeSquared() > 1.0) Move.Normalize();
	InputSentMax = FMath::Max(InputSentMax, (double)FVector2f((float)Move.X, (float)Move.Y).Size());

	const FRotator Control = PC->GetControlRotation();
	const FVector Location = Ch->GetActorLocation();
	const FVector Aim = TargetingStart(PC, Location);

	// 시작점 거리 훑기 — 서버가 부활 뒤 낡은 값으로 따로 세는 첫 30입력(Room.cpp kAimStaleInputs)과 죽은 동안은 뺀다.
	// 조준 칸은 누름 기준이라 카메라가 섞이는 0.22초는 누른 쪽 칸에 든다 — 허용 거리는 두 칸을 합친 최대로 본다
	const bool bSweepCrouchNow = Ch->IsCrouched();
	if (bSweepCrouchNow != bSweepCrouched)
	{
		bSweepCrouched = bSweepCrouchNow;
		SweepCrouchChangedAt = Now;
	}
	++InputsSinceSpawn;
	if (InputsSinceSpawn > 30 && !(ServerFlags & wire::STATE_DEAD))
	{
		FVector CamLoc;
		FRotator CamRot;
		PC->GetPlayerViewPoint(CamLoc, CamRot);
		const double CamPitch = FRotator::NormalizeAxis(CamRot.Pitch);
		const int32 Bin = FMath::Clamp(FMath::FloorToInt((CamPitch + 90.0) / 10.0), 0, 17);
		const int32 AdsSlot = (bAutoInput ? bAutoAds : PC->IsInputKeyDown(EKeys::RightMouseButton)) ? 1 : 0;
		// 앉음은 Lyra 캐릭터 상태로 가른다 — 카메라 받침점이 캡슐 반높이를 따르므로(LyraCameraMode) 시작점을 정하는 쪽이 이 값이다(D-6-a)
		const int32 Slot = AdsSlot + (Ch->IsCrouched() ? 2 : 0);
		const FVector D = Aim - Location;
		++AimSweepInputs[Slot][Bin];
		AimSweepDistMax[Slot][Bin] = FMath::Max(AimSweepDistMax[Slot][Bin], D.Size());
		AimSweepHorizMax[Slot][Bin] = FMath::Max(AimSweepHorizMax[Slot][Bin], D.Size2D());
		const int32 CrouchSlot = bSweepCrouchNow ? 1 : 0;
		if (SweepCrouchChangedAt >= 0 && Now - SweepCrouchChangedAt < 0.25)
		{
			++AimSweepFreshInputs[CrouchSlot];
			AimSweepFreshMax[CrouchSlot] = FMath::Max(AimSweepFreshMax[CrouchSlot], D.Size());
		}
		else
		{
			AimSweepSteadyMax[CrouchSlot] = FMath::Max(AimSweepSteadyMax[CrouchSlot], D.Size());
		}
		AimSweepPitchMin = FMath::Min(AimSweepPitchMin, CamPitch);
		AimSweepPitchMax = FMath::Max(AimSweepPitchMax, CamPitch);
	}
	else
	{
		++AimSweepSkipped;
	}

	const bool bFire = bAutoInput ? bAutoFire : PC->IsInputKeyDown(EKeys::LeftMouseButton);
	if (bFire)
	{
		const FVector AimDelta = Aim - Location;
		const double AimDist = AimDelta.Size();
		const double AimHoriz = AimDelta.Size2D();
		AimDistMax = FMath::Max(AimDistMax, AimDist);
		AimHorizMax = FMath::Max(AimHorizMax, AimHoriz);
		if (AimDist > 140.0 || AimHoriz > 90.0)	// 거리 = 서버 허용 거리(Room.cpp kAimMaxDistance, D66 에서 120 → 140)
		{
			++AimFarLocal;
			if (AimFarLinesLeft > 0)
			{
				--AimFarLinesLeft;
				FVector ViewLoc;
				FRotator ViewRot;
				PC->GetPlayerViewPoint(ViewLoc, ViewRot);
				UE_LOG(LogMOB, Warning, TEXT("MOB_AIM_FAR dist=%.1f horiz=%.1f pitch=%.1f ctrlPitch=%.1f camFromPawn=%.1f seq=%u"), AimDist, AimHoriz, ViewRot.Pitch, Control.Pitch, FVector::Dist(ViewLoc, Location), Seq);
			}
		}
	}
	const bool bJump = bAutoInput ? bAutoJump : PC->IsInputKeyDown(EKeys::SpaceBar);
	const bool bReloadDown = bAutoInput ? bAutoReload : PC->IsInputKeyDown(EKeys::R);
	// 앉기 = LeftControl(IMC_Default 의 IA_Crouch, 원본 기준). IA_Crouch 동작에 Pressed 트리거가 있어 Lyra 도 누를 때 한 번만 바꾼다 — 서버도 누른 입력 하나에만 바꾼다(BUTTON_CROUCH, D-5)
	const bool bCrouchDown = bAutoInput ? bAutoCrouch : PC->IsInputKeyDown(EKeys::LeftControl);
	// 조준 = 오른쪽 단추(IA_ADS, 0-1). Lyra 도 같은 키로 제 조준 능력을 그 프레임에 켜 내 카메라 · 조준점 · 시선 감도가 바뀐다 — 서버도 이 입력부터 조준이라 막지 않는다(D61)
	const bool bAds = bAutoInput ? bAutoAds : PC->IsInputKeyDown(EKeys::RightMouseButton);
	// 근접 = C · E(IMC_ShooterGame 의 IA_Melee, 0-1 원본 기준). 누르고 있는 동안 입력마다 단추를 싣는다 — 켜지는 것은 능력 간격마다(서버 · 예측 CMeleeAbility)
	const bool bMelee = bAutoInput ? bAutoMelee : (PC->IsInputKeyDown(EKeys::C) || PC->IsInputKeyDown(EKeys::E));
	// D61 확인 — 서버는 이 입력부터 조준이다. Lyra 도 같은 프레임에 최대 속도를 바꾸나(0 = 같은 프레임). 이 함수는 그 프레임 Lyra 입력 처리 뒤에 돈다
	if (const UCharacterMovementComponent* AdsMove = Ch->GetCharacterMovement())
	{
		const float MaxSpeed = AdsMove->GetMaxSpeed();
		if (bAds != bPrevAdsKey)
		{
			if (AdsKeyWatchFrame >= 0) ++AdsKeyLag[bAdsKeyWantOn ? 1 : 0][3];	// 앞 바뀜이 따라 바뀌기 전에 다시 바뀜
			bAdsKeyWantOn = bAds;
			AdsKeyBaseSpeed = PrevMaxSpeed;
			AdsKeyWatchFrame = 0;
		}
		if (AdsKeyWatchFrame >= 0)
		{
			const bool bFollowed = bAdsKeyWantOn ? MaxSpeed < AdsKeyBaseSpeed - 1.f : MaxSpeed > AdsKeyBaseSpeed + 1.f;
			if (bFollowed || AdsKeyWatchFrame >= 2)
			{
				const int32 Lag = bFollowed ? AdsKeyWatchFrame : 3;
				++AdsKeyLag[bAdsKeyWantOn ? 1 : 0][Lag];
				UE_LOG(LogMOB, Log, TEXT("MOB_ADSKEY down=%d lag=%d base=%.1f now=%.1f frame=%llu seq=%u"), bAdsKeyWantOn ? 1 : 0, Lag, AdsKeyBaseSpeed, MaxSpeed, (unsigned long long)GFrameCounter, Seq);
				AdsKeyWatchFrame = -1;
			}
			else
			{
				++AdsKeyWatchFrame;
			}
		}
		PrevMaxSpeed = MaxSpeed;
		bPrevAdsKey = bAds;
	}

	uint8 Wanted = Weapon;
	if (bAutoInput) Wanted = AutoWeapon;
	else if (PC->IsInputKeyDown(EKeys::One)) Wanted = (uint8)mo::EWeapon::Pistol;
	else if (PC->IsInputKeyDown(EKeys::Two)) Wanted = (uint8)mo::EWeapon::Rifle;
	else if (PC->IsInputKeyDown(EKeys::Three)) Wanted = (uint8)mo::EWeapon::Shotgun;
	if (Wanted != Weapon) ++InputsWeaponChange;
	Weapon = Wanted;

	// 되감기 시점 = 사람이 보고 쏜 화면. 이 함수는 TickRemotes 앞에 돌므로 ShownRenderTick 은 지난 프레임에 상대를 놓은 시점이다
	// 시계가 아직 없으면 0 = 시점 없음(서버가 현재 자리로 판정)
	uint32 ViewTick = 0;
	float ViewAlpha = 0.f;
	if (ShownRenderTick >= 1.0)
	{
		const double ViewWhole = FMath::FloorToDouble(ShownRenderTick);
		ViewTick = (uint32)ViewWhole;
		ViewAlpha = (float)(ShownRenderTick - ViewWhole);
	}

	for (int32 i = 0; i < Due; ++i)
	{
		wire::Input In = wire::Input();
		In.h.size = sizeof(In);
		In.h.type = (uint16)wire::Msg::C2S_INPUT;
		In.moveX = (float)Move.X;
		In.moveY = (float)Move.Y;
		In.yaw = FRotator::NormalizeAxis(Control.Yaw);
		In.pitch = FRotator::NormalizeAxis(Control.Pitch);
		In.aimX = (float)Aim.X;
		In.aimY = (float)Aim.Y;
		In.aimZ = (float)Aim.Z;
		In.weapon = Weapon;
		In.viewTick = ViewTick;
		In.viewAlpha = ViewAlpha;
		if (bJump) In.buttons |= wire::BUTTON_JUMP;
		if (bFire) In.buttons |= wire::BUTTON_FIRE;
		if (bReloadDown && !bReloadWasDown) { In.buttons |= wire::BUTTON_RELOAD; ++InputsReload; }
		bReloadWasDown = bReloadDown;
		if (bCrouchDown && !bCrouchWasDown) { In.buttons |= wire::BUTTON_CROUCH; ++InputsCrouch; }
		bCrouchWasDown = bCrouchDown;
		if (bAds) { In.buttons |= wire::BUTTON_ADS; ++InputsAds; }
		if (bMelee) { In.buttons |= wire::BUTTON_MELEE; ++InputsMelee; }

		// 번호를 붙이고 여기서도 돌린다 — 서버 답을 기다리지 않고 바로 움직이려고
		// 근접(E-3) — 이 입력이 근접을 켜면 넣기 전에 화면 판정을 해, 맞음이면 밀기를 같이 넣는다(봇과 같은 길, D75). Lyra 근접 능력은 TickAbilityMelee 가 켠다
		const bool bMeleeStarts = Core->Prediction.WillMeleeStart(In);
		FMOBMeleeGuess MeleeGuess;
		mo::Vec3 MeleePush;
		if (bMeleeStarts) MeleeGuess = JudgeMelee(MeleePush);
		{
			const FMOBCore::FPawnScope PawnScope(*Core);	// 화면 판정(JudgeMelee) 뒤에 넘긴다 — 봇과 같은 차례
			Core->Prediction.AddInput(In, bMeleeStarts && MeleeGuess.Kind == wire::MELEE_HIT ? &MeleePush : nullptr);
		}
		Seq = In.seq;
		MeleeYaw = In.yaw;
		if (bMeleeStarts)
		{
			MeleeGuesses.Add(In.seq, MeleeGuess);
			if (bMeleePending) ++MeleeDropped;	// 앞 근접을 아직 못 켰다 — 능력 간격보다 오래 못 켠 것
			bMeleePending = true;
			MeleePendingSeq = In.seq;
			MeleePendingFrame = GFrameCounter;
			++MeleeQueued;
		}
		const mo::WeaponEvents& PredictedFire = Core->Prediction.GetLastWeaponEvents();
		if (PredictedFire.fired && !bAbilityOwnTimer)
		{
			// 내 화면 사격 = 이 입력(D53). 능력은 TickAbilityFire 가 켠다
			FMOBPendingFire& Fire = PendingFires.AddDefaulted_GetRef();
			Fire.Seq = In.seq;
			Fire.Weapon = (uint8)Core->Prediction.GetWeaponIndex();
			Fire.SpreadAngle = PredictedFire.spreadAngle;
			Fire.SpreadMultiplier = PredictedFire.spreadMultiplier;
			Fire.AimDir = ToFVector(mo::AimDirection(In.yaw, In.pitch));
			Fire.AimFrom = FVector(In.aimX, In.aimY, In.aimZ);
			Fire.Frame = GFrameCounter;
			++FireQueued;
		}
		if (bViewLog && PredictedFire.fired)
		{
			// 서버 SPREAD 줄과 (입력 번호, 탄 번호) 로 잇는다 — 같은 입력이면 같은 방향이어야 한다(D15)
			const mo::WeaponDef& FiredDef = mo::GetWeaponDef((mo::EWeapon)Core->Prediction.GetWeaponIndex());
			const mo::Vec3 PredictedAim = mo::AimDirection(In.yaw, In.pitch);
			const int32 BulletCount = FMath::Min((int32)FiredDef.bulletsPerCartridge, (int32)wire::kMaxBulletsInShot);
			for (int32 Bullet = 0; Bullet < BulletCount; ++Bullet)
			{
				mo::CSpreadRng BulletRng(mo::SpreadSeed(In.seq, (uint32)Bullet));
				const mo::Vec3 Dir = mo::SpreadDirection(PredictedAim, PredictedFire.spreadAngle, PredictedFire.spreadMultiplier, FiredDef.spreadExponent, BulletRng);
				UE_LOG(LogMOB, Log, TEXT("MOB_PREDSHOT seq=%u i=%d frame=%llu spread=%.6f mult=%.6f aim=%.7f,%.7f,%.7f dir=%.7f,%.7f,%.7f"),
					In.seq, Bullet, (unsigned long long)GFrameCounter, PredictedFire.spreadAngle, PredictedFire.spreadMultiplier, PredictedAim.x, PredictedAim.y, PredictedAim.z, Dir.x, Dir.y, Dir.z);
			}
		}
		if (bViewLog && bFire)
		{
			// 서버 REWIND 줄과 (seq, 상대) 로 잇는다 — 서버가 되돌린 자리 = 그때 화면 자리인가
			for (const TPair<uint32, FMOBRemote>& Pair : Remotes)
			{
				const FMOBRemote& R = Pair.Value;
				const ACharacter* Shown = R.Pawn.Get();
				if (!Shown || R.bKilled) continue;
				const FVector At = Shown->GetActorLocation();
				UE_LOG(LogMOB, Log, TEXT("MOB_VIEW seq=%u target=%u viewTick=%u alpha=%.4f x=%.3f y=%.3f z=%.3f yaw=%.3f newest=%u wall=%.3f"), In.seq, R.Id, ViewTick, ViewAlpha, At.X, At.Y, At.Z, Shown->GetActorRotation().Yaw, R.ShownNewestTick, ShownWallTick);
				if (bServerAnim) LogViewParts(In.seq, R.Id, *Shown);
				if (bServerAnim && bRayCheck) CheckEngineRays(In.seq, R.Id, *Shown);
			}
		}
		if (!Core->Prediction.IsWaitingSpawn())
		{
			SelfPrev = SelfCur;
			SelfCur = ToFVector(Core->Prediction.GetMove().location);
		}
		QueuePacket(&In, sizeof(In));
		++InputsSent;
		if (bFire) ++InputsFire;
		if (bJump) ++InputsJump;
	}
}

// 내 사격 능력을 예측이 쏜 입력에서 켠다(D53). 능력이 쉬고 있으면 쌓인 발 중 가장 오래된 하나로 켠다.
// 능력이 켜진 자리에서 블루프린트가 바로 조준하므로(StartRangedWeaponTargeting) 탄 방향 훅은 이 안에서 ActiveFire 를 읽는다
void UMOBridgeSubsystem::TickAbilityFire()
{
	if (PendingFires.Num() == 0) return;
	while (PendingFires.Num() > kMaxPendingFires)
	{
		PendingFires.RemoveAt(0);
		++FireDropped;
	}
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	ULyraAbilitySystemComponent* LASC = Cast<ULyraAbilitySystemComponent>(FindASC(PC ? PC->GetPawn() : nullptr));
	// 권총 능력은 Fire, 소총 · 샷건 능력은 FireAuto 입력에 묶여 있다(MOBWeaponRecordSubsystem 과 같은 두 표식)
	static const FGameplayTag FireTag = FGameplayTag::RequestGameplayTag(TEXT("InputTag.Weapon.Fire"));
	static const FGameplayTag FireAutoTag = FGameplayTag::RequestGameplayTag(TEXT("InputTag.Weapon.FireAuto"));
	FGameplayAbilitySpecHandle Handle;
	bool bActive = false;
	if (LASC)
	{
		for (const FGameplayAbilitySpec& Spec : LASC->GetActivatableAbilities())
		{
			if (Spec.Ability && Spec.Ability->IsA<ULyraGameplayAbility_RangedWeapon>() && (Spec.GetDynamicSpecSourceTags().HasTagExact(FireTag) || Spec.GetDynamicSpecSourceTags().HasTagExact(FireAutoTag)))
			{
				Handle = Spec.Handle;
				bActive = Spec.IsActive();
				break;
			}
		}
	}
	if (!Handle.IsValid())
	{
		// 사격 능력이 없다(캐릭터가 없거나 무기를 갈아 끼우는 중) — 이 발은 그리지 못한다
		PendingFires.RemoveAt(0);
		++FireNoSpec;
		return;
	}
	if (bActive)
	{
		++FireWaitFrames;
		return;
	}
	ActiveFire = PendingFires[0];
	PendingFires.RemoveAt(0);
	bActiveFireValid = true;
	if (ActiveFire.Weapon != LastSyncWeapon) ++FireWeaponMismatch;
	++FireLagFrames[(int32)FMath::Min<uint64>(GFrameCounter - ActiveFire.Frame, 3)];
	bAbilityActivating = true;
	const bool bActivated = LASC->TryActivateAbility(Handle);
	bAbilityActivating = false;
	if (bActivated) ++FireDriven;
	else ++FireActivateFail;
}

bool UMOBridgeSubsystem::AllowAbilityActivation()
{
	if (bAbilityActivating) return true;
	// 탄창이 비었으면 원래 검사로 넘긴다 — 세 사격 능력의 비용이 탄창이라(LyraAbilityCost_ItemTagStack · MagazineAmmo, 에셋 문자열 기준) 0이면 반드시 막히고,
	// Lyra 가 탄 부족 표식(Ability.ActivateFail.Cost)에 건 빈총 몽타주 · 문구가 난다. 막기만 하면 빈총 동작이 사라진다
	static const FGameplayTag MagTag = FGameplayTag::RequestGameplayTag(TEXT("Lyra.ShooterGame.Weapon.MagazineAmmo"));
	// 탄창은 든 무기(빠른 슬롯의 지금 칸)에서 읽는다 — 사격 능력은 그 무기가 준 것이다. 능력의 GetAssociatedItem 은 Lyra 모듈 밖에 안 열려 있다
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	ULyraQuickBarComponent* Bar = PC ? PC->FindComponentByClass<ULyraQuickBarComponent>() : nullptr;
	const ULyraInventoryItemInstance* Item = Bar ? Bar->GetActiveSlotItem() : nullptr;
	if (Item && Item->GetStatTagStackCount(MagTag) <= 0)
	{
		++FireGateDryFire;
		return true;
	}
	++FireGateBlocks;
	return false;
}

// 근접 화면 판정(E-3 17-c) — 서버 CRoom::TryMelee 와 같은 식, 봇 JudgeMelee 와 같은 짜임(D75). 손 = 걸음 앞 예측 자리 · 앞 입력의 요 · 기본 자세 손(앉았으면 메시 올림).
// 상대 = 지난 프레임에 화면에 놓은 자리 · 요 — 이 입력이 싣는 시점(viewTick · viewAlpha)이 그 자리이고 서버가 그 시점으로 되감는다. 캡슐 높이 = 화면 캐릭터 앉음(앞 틱 앉음, D-5-c),
// 몸 = 서 있는 메시 몸체. 클립을 든 서버는 자세 손 · 자세 몸으로 판정해 갈릴 수 있다 — 갈리면 서버 결과로 고친다(HandleMelee → OnMeleeResult, D74)
FMOBMeleeGuess UMOBridgeSubsystem::JudgeMelee(mo::Vec3& OutPush)
{
	++MeleeJudged;
	FMOBMeleeGuess Guess;
	const mo::MoveState& Move = Core->Prediction.GetMove();
	double Local[3];
	mo::MeleeRestHandLocal(Move.crouched, Core->Sim.CrouchMeshLiftZ(), Local);
	const mo::Vec3 Hand = mo::MeleeHandWorld(Move.location, MeleeYaw, Local);
	const mo::Vec3 Forward = mo::AimDirection(MeleeYaw, 0.f);
	std::vector<mo::MeleeTarget> Targets;
	for (const TPair<uint32, FMOBRemote>& Pair : Remotes)
	{
		const FMOBRemote& R = Pair.Value;
		const ACharacter* Shown = R.Pawn.Get();
		if (!Shown || R.bKilled) continue;		// 서버 — 지금 죽어 있으면 그때 살아 있었어도 안 맞는다
		const FVector At = Shown->GetActorLocation();
		mo::MeleeTarget Target;
		Target.body.id = R.Id;
		Target.body.center = mo::Vec3(At.X, At.Y, At.Z);
		Target.body.yawDegrees = Shown->GetActorRotation().Yaw;
		Target.capsuleRadius = Core->Params.capsuleRadius;
		Target.capsuleHalfHeight = Shown->IsCrouched() ? Core->Params.capsuleHalfHeight - Core->Sim.CrouchMeshLiftZ() : Core->Params.capsuleHalfHeight;	// 서버 TryMelee 와 같은 식
		Target.sameTeam = R.ServerTeam == Team;
		Targets.push_back(Target);
	}
	mo::MeleeResult Result;
	mo::ResolveMelee(Core->Map, Targets.data(), Targets.size(), Hand, Forward, Result);
	Guess.Kind = (uint8)Result.kind;		// EMeleeHit 차례 = wire::EMeleeResult 차례(Room.cpp static_assert)
	if (Result.kind != mo::EMeleeHit::None) Guess.Target = Targets[Result.targetIndex].body.id;
	Guess.Distance = (float)Result.distance;		// 서버도 맞음이 아닐 때 거리를 싣는다(같은 팀 · 벽)
	++MeleeScreen[FMath::Min<int32>(Guess.Kind, 3)];
	if (Result.kind == mo::EMeleeHit::Hit)
	{
		// 서버와 같이 와이어 float 로 줄인 힘 — 같으면 서버 결과가 와도 다시 돌릴 일이 없다
		const mo::Vec3 Exact = mo::MeleePushForce(Forward, Result.distance);
		Guess.ForceX = (float)Exact.x;
		Guess.ForceY = (float)Exact.y;
		OutPush = mo::Vec3(Guess.ForceX, Guess.ForceY, 0.0);
	}
	return Guess;
}

// 근접 소식(E-3 17-c) — 내 근접이면 화면 판정과 견주고 예측 밀기를 서버 값으로 맞춘다(OnMeleeResult — 다르면 그 입력부터 걸음을 다시 돌린다).
// 서버는 이 소식을 그 입력을 돈 틱의 상태보다 먼저 보낸다(같은 연결). 남의 근접 몽타주 · 맞은 쪽 반응은 서버 애니 상태로, 피해 · 처치는 S2C_DAMAGE · S2C_KILL 로 온다
void UMOBridgeSubsystem::HandleMelee(const wire::Melee& M)
{
	if (M.attacker != PlayerId)
	{
		++MeleeOthers;
		return;
	}
	const bool bHit = M.result == wire::MELEE_HIT;
	++MeleeMine[FMath::Min<int32>(M.result, 3)];
	if (const FMOBMeleeGuess* Guess = MeleeGuesses.Find(M.seq))
	{
		if (Guess->Kind != M.result) ++MeleeKindDiff;
		else if (Guess->Target != M.target) ++MeleeTargetDiff;
		const double Fx = (double)Guess->ForceX - (bHit ? (double)M.forceX : 0.0);
		const double Fy = (double)Guess->ForceY - (bHit ? (double)M.forceY : 0.0);
		const double ForceDiff = FMath::Sqrt(Fx * Fx + Fy * Fy);
		MeleeForceDiffMax = FMath::Max(MeleeForceDiffMax, ForceDiff);
		if (MeleeLinesLeft > 0)
		{
			--MeleeLinesLeft;
			UE_LOG(LogMOB, Log, TEXT("MOB_MELEE_ONE seq=%u screen=%u target=%u dist=%.2f server=%u target=%u dist=%.2f forceDiff=%.3f damage=%.0f weapon=%u"),
				M.seq, Guess->Kind, Guess->Target, Guess->Distance, M.result, M.target, M.distance, ForceDiff, M.damage, M.weapon);
		}
		MeleeGuesses.Remove(M.seq);
	}
	else
	{
		++MeleeNoGuess;
	}
	if (Core)
	{
		const FMOBCore::FPawnScope PawnScope(*Core);	// 그 입력부터 다시 돌리는 걸음도 남 캡슐을 본다 — 근접은 늘 남 옆이다
		Core->Prediction.OnMeleeResult(M.seq, bHit, mo::Vec3((double)M.forceX, (double)M.forceY, 0.0));
	}
}

// Lyra 근접 능력(GA_Melee)을 예측이 켠 근접 입력에서 켠다(사격 TickAbilityFire 와 같은 꼴, D77). 능력 칸의 입력 표식(InputTag.Ability.Melee)을 떼어
// C · E 키로는 Lyra 가 혼자 못 켠다(ULyraAbilitySystemComponent::AbilityInputTagPressed 가 이 표식으로 칸을 찾는다). 캐릭터가 새로 생기면 능력 칸을 새로 받으므로 프레임마다 찾아 뗀다.
// 켠 능력이 내 공격 몽타주 · 소리 · 맞은 연출을 낸다. 피해는 막혀 있고(BlockLyraDamage), 밀기는 Lyra 이동 계산이 쉬어(NoLyraMove) 내 자리를 안 바꾼다 — 자리는 예측 밀기다
void UMOBridgeSubsystem::TickAbilityMelee()
{
	static const FGameplayTag MeleeInputTag = FGameplayTag::RequestGameplayTag(TEXT("InputTag.Ability.Melee"), false);
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	ULyraAbilitySystemComponent* LASC = Cast<ULyraAbilitySystemComponent>(FindASC(PC ? PC->GetPawn() : nullptr));
	const FGameplayAbilitySpec* Spec = nullptr;
	if (LASC && MeleeInputTag.IsValid())
	{
		for (FGameplayAbilitySpec& Each : LASC->GetActivatableAbilities())
		{
			if (!Each.Ability || !Each.GetDynamicSpecSourceTags().HasTagExact(MeleeInputTag)) continue;
			if (!bBreakMeleeBlock || GMeleeSpecHandle != Each.Handle) UE_LOG(LogMOB, Log, TEXT("MOB_MELEE_SPEC ability=%s handle=%s strip=%d"), *GetNameSafe(Each.Ability), *Each.Handle.ToString(), bBreakMeleeBlock ? 0 : 1);
			GMeleeSpecHandle = Each.Handle;
			if (!bBreakMeleeBlock)
			{
				Each.GetDynamicSpecSourceTags().RemoveTag(MeleeInputTag);
				LASC->MarkAbilitySpecDirty(Each);
				++LyraMeleeStripped;
			}
			break;
		}
		Spec = GMeleeSpecHandle.IsValid() ? LASC->FindAbilitySpecFromHandle(GMeleeSpecHandle) : nullptr;
	}
	const bool bActive = Spec && Spec->IsActive();
	if (bActive && !bLyraMeleeWasActive)
	{
		// 지난 프레임 끝엔 꺼져 있었는데 다리가 켜기 전에 켜졌다 = Lyra 가 혼자 켰다(다리가 켠 것은 아래에서 켠 뒤 상태를 적어 여기 안 든다)
		if (LyraMeleeSelf < 10) UE_LOG(LogMOB, Log, TEXT("MOB_MELEE_LYRA_SELF frame=%llu seq=%u"), (unsigned long long)GFrameCounter, Seq);
		++LyraMeleeSelf;
	}
	if (bMeleePending)
	{
		if (!Spec)
		{
			bMeleePending = false;
			++MeleeNoSpec;
		}
		else if (bActive)
		{
			++MeleeWaitFrames;
			if (GFrameCounter - MeleePendingFrame > kMaxMeleeWaitFrames)
			{
				bMeleePending = false;
				++MeleeDropped;
			}
		}
		else
		{
			bMeleePending = false;
			if (LASC->TryActivateAbility(GMeleeSpecHandle)) ++MeleeDriven;
			else ++MeleeActivateFail;
		}
	}
	const FGameplayAbilitySpec* After = LASC && GMeleeSpecHandle.IsValid() ? LASC->FindAbilitySpecFromHandle(GMeleeSpecHandle) : nullptr;
	bLyraMeleeWasActive = After && After->IsActive();
	// 결과 없이 지나간 근접의 화면 판정(서버가 안 켬 · 기다리는 중 죽음)은 오래되면 버린다 — 600입력 = 10초(제 판단)
	if (MeleeGuesses.Num() > 16)
	{
		for (auto It = MeleeGuesses.CreateIterator(); It; ++It)
		{
			if (It.Key() + 600 < Seq) It.RemoveCurrent();
		}
	}
}

// 화면의 탄 방향 = 서버와 같은 식(D15). 입력 번호 · 조준 · 퍼짐은 다리가 켠 사격(ActiveFire)의 것을 쓴다(D53) — 능력이 잰 조준 · 시작점 · 퍼짐은 견주기로만 남긴다
bool UMOBridgeSubsystem::ComputeBulletDirection(const FVector& StartTrace, const FVector& AimDir, float SpreadAngle, float SpreadMultiplier, float SpreadExponent, int32 BulletIndex, FVector& OutDir)
{
	if (!Core || !bActiveFireValid) return false;
	++FireBullets;
	if (!bAbilityActivating) ++FireBulletsOutside;
	const mo::WeaponDef& FiredDef = mo::GetWeaponDef((mo::EWeapon)ActiveFire.Weapon);
	const mo::Vec3 Aim(ActiveFire.AimDir.X, ActiveFire.AimDir.Y, ActiveFire.AimDir.Z);
	mo::CSpreadRng BulletRng(mo::SpreadSeed(ActiveFire.Seq, (uint32)BulletIndex));
	const mo::Vec3 Dir = mo::SpreadDirection(Aim, ActiveFire.SpreadAngle, ActiveFire.SpreadMultiplier, FiredDef.spreadExponent, BulletRng);
	OutDir = ToFVector(Dir);
	const double LyraAimDeg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(AimDir.GetSafeNormal(), ActiveFire.AimDir.GetSafeNormal()), -1.0, 1.0)));
	const double StartDiff = FVector::Dist(StartTrace, ActiveFire.AimFrom);
	FireLyraAimMaxDeg = FMath::Max(FireLyraAimMaxDeg, LyraAimDeg);
	FireStartDiffMax = FMath::Max(FireStartDiffMax, StartDiff);
	if (bViewLog)
	{
		// check_spread.py 가 읽는 앞부분(seq ~ dir)은 MOB_PREDSHOT 과 같은 꼴 — 뒤는 능력이 잰 값과의 차이 · 능력의 퍼짐
		UE_LOG(LogMOB, Log, TEXT("MOB_SHOTDIR seq=%u i=%d frame=%llu spread=%.6f mult=%.6f aim=%.7f,%.7f,%.7f dir=%.7f,%.7f,%.7f lyraAimDeg=%.4f startDiff=%.2f lyraSpread=%.6f lyraMult=%.6f"),
			ActiveFire.Seq, BulletIndex, (unsigned long long)GFrameCounter, ActiveFire.SpreadAngle, ActiveFire.SpreadMultiplier, Aim.x, Aim.y, Aim.z, Dir.x, Dir.y, Dir.z, LyraAimDeg, StartDiff, SpreadAngle, SpreadMultiplier);
	}
	return true;
}

// 사람 없이 시험하는 입력 — 시선을 돌리며 앞·왼·뒤·오른으로 걷고, 점프·사격·재장전·무기 바꾸기를 번갈아 낸다
// 쏠 때 화면에 그려진 22부위를 몸 기준 좌표(캡슐 중심, 요 0)로 남긴다 — 서버 REWINDP 줄과 (seq, 상대, 부위) 로 잇는다.
// 이 함수는 TickRemotes 앞에 돌므로 메시의 뼈는 지난 프레임에 그린 자세 = 사람이 보고 쏜 자세다. 형식은 POSEB 와 같은 뼈 자리
void UMOBridgeSubsystem::LogViewParts(uint32 InSeq, uint32 Target, const ACharacter& Shown)
{
	const USkeletalMeshComponent* Mesh = Shown.GetMesh();
	const UPhysicsAsset* PA = Mesh ? Mesh->GetPhysicsAsset() : nullptr;
	if (!PA) return;
	const FVector Loc = Shown.GetActorLocation();
	const FQuat ActorInv = FRotator(0, Shown.GetActorRotation().Yaw, 0).Quaternion().Inverse();
	for (const USkeletalBodySetup* B : PA->SkeletalBodySetups)
	{
		if (!B) continue;
		const int32 BoneIndex = Mesh->GetBoneIndex(B->BoneName);
		if (BoneIndex == INDEX_NONE) continue;
		FTransform BoneW = Mesh->GetBoneTransform(BoneIndex);
		BoneW.SetScale3D(FVector::OneVector);
		const auto Emit = [&](const FTransform& ElemLocal)
		{
			const FTransform W = ElemLocal * BoneW;
			const FVector P = ActorInv.RotateVector(W.GetLocation() - Loc);
			const FVector X = ActorInv.RotateVector(W.GetUnitAxis(EAxis::X));
			const FVector Y = ActorInv.RotateVector(W.GetUnitAxis(EAxis::Y));
			const FVector Z = ActorInv.RotateVector(W.GetUnitAxis(EAxis::Z));
			UE_LOG(LogMOB, Log, TEXT("MOB_VIEWP seq=%u target=%u bone=%s p=%.4f,%.4f,%.4f x=%.5f,%.5f,%.5f y=%.5f,%.5f,%.5f z=%.5f,%.5f,%.5f"), InSeq, Target, *B->BoneName.ToString(), P.X, P.Y, P.Z, X.X, X.Y, X.Z, Y.X, Y.Y, Y.Z, Z.X, Z.Y, Z.Z);
		};
		for (const FKSphereElem& E : B->AggGeom.SphereElems) Emit(FTransform(E.Center));
		for (const FKSphylElem& E : B->AggGeom.SphylElems) Emit(FTransform(E.Rotation, E.Center));
		for (const FKBoxElem& E : B->AggGeom.BoxElems) Emit(FTransform(E.Rotation, E.Center));
	}
}

// 같은 자세에 엔진 광선 ↔ TraceBody. 엔진이 실제로 쏘는 물리 몸체 자리로 자세를 만들어 같은 광선을 양쪽에 쏜다.
// 광선을 고르는 식은 무기 기록의 RAY 줄과 같고(MOBWeaponRecordSubsystem::TickRays), 머리 쪽을 겨눈 절반을 더했다 — 머리 합격선을 보려면 머리에 맞는 발이 충분해야 한다
void UMOBridgeSubsystem::CheckEngineRays(uint32 InSeq, uint32 Target, const ACharacter& Shown)
{
	UWorld* World = GetWorld();
	const USkeletalMeshComponent* Mesh = Shown.GetMesh();
	if (!World || !Mesh || !Core) return;
	const std::vector<mo::ClipBody>& Shapes = Core->AnimClips.Bodies();
	const FVector Loc = Shown.GetActorLocation();
	const double Yaw = Shown.GetActorRotation().Yaw;
	const FQuat ActorInv = FRotator(0, Yaw, 0).Quaternion().Inverse();
	std::vector<mo::BodyPose> Poses(Shapes.size());
	FVector Head = Loc;
	for (size_t I = 0; I < Shapes.size(); ++I)
	{
		const mo::ClipBody& S = Shapes[I];
		const FName BoneName = Core->AnimBoneNames[S.bone];
		const FBodyInstance* Inst = Mesh->GetBodyInstance(BoneName);
		if (!Inst || !Inst->IsValidBodyInstance())
		{
			++RayNoBody;
			return;
		}
		FTransform BodyW = Inst->GetUnrealWorldTransform();
		BodyW.SetScale3D(FVector::OneVector);
		const FTransform W = FTransform(FQuat(S.q[0], S.q[1], S.q[2], S.q[3]), FVector(S.center[0], S.center[1], S.center[2])) * BodyW;
		const FVector P = ActorInv.RotateVector(W.GetLocation() - Loc);
		const FVector X = ActorInv.RotateVector(W.GetUnitAxis(EAxis::X));
		const FVector Y = ActorInv.RotateVector(W.GetUnitAxis(EAxis::Y));
		const FVector Z = ActorInv.RotateVector(W.GetUnitAxis(EAxis::Z));
		mo::BodyPose& Out = Poses[I];
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			Out.center[Axis] = P[Axis];
			Out.axisX[Axis] = X[Axis];
			Out.axisY[Axis] = Y[Axis];
			Out.axisZ[Axis] = Z[Axis];
		}
		if (BoneName == FName(TEXT("head"))) Head = W.GetLocation();
	}

	mo::BodyPlacement Body;
	Body.center = mo::Vec3(Loc.X, Loc.Y, Loc.Z);
	Body.yawDegrees = RayBreak == TEXT("yaw") ? -Yaw : Yaw;
	Body.pose = Poses.data();
	Body.shapes = Shapes.data();
	Body.shapeCount = (uint32)Shapes.size();

	FCollisionQueryParams Params(SCENE_QUERY_STAT(MOBRayCheck), /*bTraceComplex=*/ true);
	const APlayerController* PC = World->GetFirstPlayerController();
	if (PC && PC->GetPawn()) Params.AddIgnoredActor(PC->GetPawn());
	for (const TPair<uint32, FMOBRemote>& Pair : Remotes)
	{
		const ACharacter* Other = Pair.Value.Pawn.Get();
		if (Other && Other != &Shown) Params.AddIgnoredActor(Other);
	}

	const auto ShapeOf = [&](FName Bone) -> int32
	{
		for (size_t I = 0; I < Shapes.size(); ++I)
		{
			if (Core->AnimBoneNames[Shapes[I].bone] == Bone) return (int32)I;
		}
		return INDEX_NONE;
	};

	constexpr int32 RaysPerTarget = 40;
	static const double Radii[] = { 0.0, 5.5 };	// 선 · 소총 구
	for (int32 Ray = 0; Ray < RaysPerTarget; ++Ray)
	{
		const int32 AimHead = Ray & 1;
		const FVector Aim = AimHead
			? Head + RayRng.GetUnitVector() * (60.0 * FMath::Pow((double)RayRng.FRand(), 1.0 / 3.0))
			: Loc + FVector(RayRng.FRandRange(-50.f, 50.f), RayRng.FRandRange(-50.f, 50.f), RayRng.FRandRange(-95.f, 95.f));
		const FVector Dir = (RayRng.GetUnitVector() * FVector(1.0, 1.0, 0.5)).GetSafeNormal();
		const double Len = RayRng.FRandRange(100.f, 3000.f);
		const FVector A = Aim - Dir * Len;
		const FVector B = Aim + Dir * 200.0;

		for (int32 RadiusIndex = 0; RadiusIndex < 2; ++RadiusIndex)
		{
			const double R = Radii[RadiusIndex];
			FMOBRayStats& Stats = RayStats[RadiusIndex][AimHead];
			TArray<FHitResult> Hits;
			if (R > 0.0) World->SweepMultiByChannel(Hits, A, B, FQuat::Identity, Lyra_TraceChannel_Weapon, FCollisionShape::MakeSphere((float)R), Params);
			else World->LineTraceMultiByChannel(Hits, A, B, Lyra_TraceChannel_Weapon, Params);
			if (Hits.Num() > 0 && Hits[0].GetActor() != &Shown)
			{
				++Stats.Map;		// 맵이 먼저 맞은 광선은 몸 판정 대조가 아니다(D28 과 같이 뺀다)
				continue;
			}
			++Stats.Rays;
			const bool bEng = Hits.Num() > 0;
			const FName EngBone = bEng ? Hits[0].BoneName : NAME_None;
			const int32 EngShape = bEng ? ShapeOf(EngBone) : INDEX_NONE;
			const bool bEngWeak = EngShape != INDEX_NONE && Shapes[EngShape].weakSpot;

			mo::BodyHit Hit;
			const bool bMine = mo::TraceBody(Body, mo::Vec3(A.X, A.Y, A.Z), mo::Vec3(B.X, B.Y, B.Z), RayBreak == TEXT("nothick") ? 0.0 : R, Hit);
			const bool bMineWeak = bMine && Hit.weakSpot;
			const FName MineBone = bMine ? Core->AnimBoneNames[Shapes[Hit.shape].bone] : NAME_None;

			Stats.EngHit += bEng;
			Stats.EngWeak += bEngWeak;
			if (bEng == bMine) ++Stats.Same;
			else if (bEng) ++Stats.EngOnly;
			else ++Stats.MineOnly;
			if (bEng && bMine)
			{
				++Stats.Both;
				Stats.BoneSame += EngBone == MineBone;
			}
			Stats.WeakLost += bEngWeak && !bMineWeak;
			Stats.WeakGain += !bEngWeak && bMineWeak;
			if ((bEng != bMine || bEngWeak != bMineWeak) && RayDiffLogged < 200)
			{
				++RayDiffLogged;
				UE_LOG(LogMOB, Log, TEXT("MOB_RAYDIFF seq=%u target=%u r=%.1f aimHead=%d eng=%s mine=%s engWeak=%d mineWeak=%d"), InSeq, Target, R, AimHead, *EngBone.ToString(), *MineBone.ToString(), bEngWeak ? 1 : 0, bMineWeak ? 1 : 0);
			}
		}
	}
}

void UMOBridgeSubsystem::TickAutoInput(double T)
{
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	ACharacter* Ch = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
	if (!Ch || !bHasSpawn || T < kAutoStart) return;

	const double S = T - kAutoStart;
	static const FVector Dirs[8] = { FVector(1, 0, 0), FVector(1, -1, 0), FVector(0, -1, 0), FVector(-1, -1, 0), FVector(-1, 0, 0), FVector(-1, 1, 0), FVector(0, 1, 0), FVector(1, 1, 0) };
	FVector AutoMove = Dirs[(int32)(S / kAutoLeg) % 8];
	FRotator AutoRot(-5.f, (float)FMath::Fmod(S * 20.0, 360.0), 0.f);
	// 시작점 거리 훑기(C-5) — 시선을 위아래 끝까지 9초 주기로 흔든다. 조준(7초) · 사격(5초) 주기와 엇갈려 각도마다 조준한 입력 · 안 한 입력이 다 나온다.
	// Lyra 카메라가 높낮이를 ±89 로 자른다(LYRA_CAMERA_DEFAULT_PITCH_MIN/MAX)
	if (bAutoPitchSweep) AutoRot.Pitch = (float)(89.0 * FMath::Sin(2.0 * UE_DOUBLE_PI * S / 9.0));
	double AutoNearestEnemy = -1.0;
	if (bAutoAim)
	{
		const FVector From = Ch->GetActorLocation();
		const ACharacter* Best = nullptr;
		const ACharacter* Nearest = nullptr;
		double BestDist = 0, NearestDist = 0;
		for (const TPair<uint32, FMOBRemote>& Pair : Remotes)
		{
			const FMOBRemote& R = Pair.Value;
			if (R.ServerTeam == Team || R.bKilled || !R.Pawn.IsValid()) continue;
			const double D = FVector::DistSquared(From, R.Pawn->GetActorLocation());
			if (!Nearest || D < NearestDist)
			{
				Nearest = R.Pawn.Get();
				NearestDist = D;
			}
			if (PC->LineOfSightTo(R.Pawn.Get()) && (!Best || D < BestDist))
			{
				Best = R.Pawn.Get();
				BestDist = D;
			}
		}
		bAutoSeesEnemy = Best != nullptr;
		AutoNearestEnemy = Nearest ? FMath::Sqrt(NearestDist) : -1.0;
		// 안 보이면 다가간다 — 벽을 돌아가지는 않으므로 네 방향 걷기와 번갈아 끼임을 푼다.
		// 근접(-MOBAutoMelee)이면 10m 안에 보여도 다가간다(봇 --melee 가 옆걸음을 끄는 것과 같다 — 안 그러면 근접 거리 4m 에 거의 안 든다)
		if (Nearest && FMath::Fmod(S, 6.0) >= 2.0 && (bAutoMeleeOn || !(Best && BestDist < 1000000.0)))
		{
			AutoMove = (Nearest->GetActorLocation() - From).GetSafeNormal2D();
		}
		if (Best)
		{
			AutoRot = (Best->GetActorLocation() + FVector(0, 0, 30) - TargetingStart(PC, From)).Rotation();
			if (T >= NextSeeShotAt)
			{
				NextSeeShotAt = T + 4.0;
				RequestEventShot(TEXT("see"), 0.3, 4);
			}
		}
	}
	PC->SetControlRotation(AutoRot);
	// 사람 입력처럼 앞·옆을 따로 넣는다 — 대각선이면 합친 길이가 1 을 넘는 경로를 그대로 탄다
	Ch->AddMovementInput(FVector::ForwardVector, (float)AutoMove.X);
	Ch->AddMovementInput(FVector::RightVector, (float)AutoMove.Y);

	const bool bJumpNow = FMath::Fmod(S, 5.0) < 0.1;
	if (bJumpNow && !bAutoJump) Ch->Jump();
	if (!bJumpNow && bAutoJump) Ch->StopJumping();
	bAutoJump = bJumpNow;
	// 앉기 — 3.5초마다 한 번 누름(주기는 제 판단). 사격 · 점프(5초) · 조준(7초) 주기와 엇갈려 앉아 쏜 발 · 앉아 조준 · 앉은 채 점프(일어서며 뜀)가 다 나온다.
	// 사람 키처럼 Lyra 앉기 함수에도 누름을 넣는다(키에 묶인 ULyraHeroComponent::Input_Crouch → ToggleCrouch). 막기 갈고리가 되돌려야 한다 — MOB_CROUCH lyraBlocked 로 센다
	const bool bCrouchNow = FMath::Fmod(S, 3.5) < 0.1;
	if (bCrouchNow && !bAutoCrouch)
	{
		if (ALyraCharacter* LyraCh = Cast<ALyraCharacter>(Ch)) LyraCh->ToggleCrouch();
	}
	bAutoCrouch = bCrouchNow;

	const bool bImmuneNow = MatchPhase == (uint8)wire::EMatchPhase::Playing && (ServerFlags & wire::STATE_IMMUNE) && !(ServerFlags & wire::STATE_DEAD);
	const bool bFireNow = bImmuneNow || (bAutoAim ? bAutoSeesEnemy : FMath::Fmod(S, 5.0) >= 2.0 && FMath::Fmod(S, 5.0) < 4.0);
	if (bFireNow != bAutoFire)
	{
		// 사람이 누른 것과 같은 길 — 내 총의 불꽃·궤적은 Lyra 능력이 그린다. 피해는 막혀 있다
		static const FGameplayTag FireTag = FGameplayTag::RequestGameplayTag(TEXT("InputTag.Weapon.Fire"));
		if (ULyraAbilitySystemComponent* LASC = Cast<ULyraAbilitySystemComponent>(FindASC(Ch)))
		{
			if (bFireNow) LASC->AbilityInputTagPressed(FireTag);
			else LASC->AbilityInputTagReleased(FireTag);
		}
	}
	bAutoFire = bFireNow;
	// 조준 — 자동 조준 판은 적이 보이는 동안, 아니면 7초마다 3초. 사격 창(5초마다 2초)과 주기를 엇갈려 조준한 발 · 안 한 발 · 쏘는 중 켜고 끄기가 다 나오게 한다.
	// 사람 키처럼 Lyra 능력에도 누름을 넣어 내 카메라가 같이 바뀐다 — 눌린 동안 매 프레임(IA_ADS 트리거 없음 → Triggered 마다 Pressed, 원본 기준)
	const bool bAdsNow = bAutoAim ? bAutoSeesEnemy : FMath::Fmod(S, 7.0) >= 1.0 && FMath::Fmod(S, 7.0) < 4.0;
	{
		static const FGameplayTag AdsTag = FGameplayTag::RequestGameplayTag(TEXT("InputTag.Weapon.ADS"), false);
		ULyraAbilitySystemComponent* LASC = Cast<ULyraAbilitySystemComponent>(FindASC(Ch));
		if (LASC && AdsTag.IsValid())
		{
			if (bAdsNow) LASC->AbilityInputTagPressed(AdsTag);
			else if (bAutoAds) LASC->AbilityInputTagReleased(AdsTag);
		}
	}
	bAutoAds = bAdsNow;
	// 근접(-MOBAutoMelee) — 적이 4m 안이면 누르고 있고(봇 --melee 와 같은 거리), 아니면 6초마다 0.1초(빗나감도 나오게, 제 판단).
	// 사람 키처럼 Lyra 입력 표식에도 누름을 넣는다 — 다리가 표식을 뗐으면 아무 일도 없어야 하고, 안 뗐으면(-MOBBreakMeleeBlock) Lyra 가 혼자 켠다
	const bool bMeleeNow = bAutoMeleeOn && ((AutoNearestEnemy >= 0.0 && AutoNearestEnemy < 400.0) || FMath::Fmod(S, 6.0) < 0.1);
	{
		static const FGameplayTag MeleeTag = FGameplayTag::RequestGameplayTag(TEXT("InputTag.Ability.Melee"), false);
		ULyraAbilitySystemComponent* LASC = Cast<ULyraAbilitySystemComponent>(FindASC(Ch));
		if (LASC && MeleeTag.IsValid())
		{
			if (bMeleeNow) LASC->AbilityInputTagPressed(MeleeTag);
			else if (bAutoMelee) LASC->AbilityInputTagReleased(MeleeTag);
		}
	}
	bAutoMelee = bMeleeNow;
	bAutoReload = FMath::Fmod(S, 20.0) >= 19.0 || (bAutoAim && ServerMagazine == 0);
	if (bAutoReload != bAutoReloadPressed)
	{
		static const FGameplayTag ReloadTag = FGameplayTag::RequestGameplayTag(TEXT("InputTag.Weapon.Reload"), false);
		ULyraAbilitySystemComponent* LASC = Cast<ULyraAbilitySystemComponent>(FindASC(Ch));
		if (LASC && ReloadTag.IsValid())
		{
			if (bAutoReload) LASC->AbilityInputTagPressed(ReloadTag);
			else LASC->AbilityInputTagReleased(ReloadTag);
		}
		bAutoReloadPressed = bAutoReload;
	}
	AutoWeapon = AutoWeaponFixed >= 0 ? (uint8)AutoWeaponFixed : (uint8)((int32)(S / 10.0) % (int32)mo::EWeapon::Count);
}

// ─────────────────────────────────────────────────────────────
//  캡처
// ─────────────────────────────────────────────────────────────

void UMOBridgeSubsystem::RequestEventShot(const TCHAR* Name, double Delay, int32 MaxCount)
{
	if (!bEventShots) return;
	int32& Count = EventShotCounts.FindOrAdd(Name);
	if (Count >= MaxCount) return;
	++Count;
	PendingEventShots.Emplace(FPlatformTime::Seconds() + Delay, FString::Printf(TEXT("%s_%s_%d.png"), *Tag, Name, Count));
}

void UMOBridgeSubsystem::TickCapture(double T)
{
	const FString Dir = FPaths::ProjectSavedDir() / TEXT("MOB");
	// 한 프레임에 한 장 — 요청이 겹치면 앞의 것이 덮인다
	for (int32 i = 0; i < PendingEventShots.Num(); ++i)
	{
		if (FPlatformTime::Seconds() < PendingEventShots[i].Key) continue;
		const FString Path = Dir / PendingEventShots[i].Value;
		FScreenshotRequest::RequestScreenshot(Path, true, false);
		UE_LOG(LogMOB, Log, TEXT("MOB_SHOT t=%.2f path=%s"), T, *Path);
		PendingEventShots.RemoveAt(i);
		return;
	}
	if (ShotIndex < ShotAt.Num() && T >= ShotAt[ShotIndex])
	{
		const FString Path = Dir / FString::Printf(TEXT("%s_%02d.png"), *Tag, FMath::RoundToInt(ShotAt[ShotIndex]));
		FScreenshotRequest::RequestScreenshot(Path, true, false);
		UE_LOG(LogMOB, Log, TEXT("MOB_SHOT t=%.2f path=%s"), T, *Path);
		++ShotIndex;
	}
	TickRecord(T);
}

// 영상 — 원시 BGRA 프레임을 한 파일(Saved/MOB/rec_<tag>.bgra)에 이어 쓴다. mp4 는 바깥에서 ffmpeg 로 만든다(MOB_REC 줄에 크기·프레임 수·초)
void UMOBridgeSubsystem::TickRecord(double T)
{
	if (RecStart < 0 || bGrabDone) return;
	if (!Grabber && T >= RecStart && T <= RecStart + RecDur)
	{
		UGameEngine* GE = Cast<UGameEngine>(GEngine);
		TSharedPtr<FSceneViewport> SV = GE ? GE->SceneViewport : nullptr;
		if (!SV.IsValid()) { UE_LOG(LogMOB, Error, TEXT("MOB_REC_FAIL no scene viewport")); bGrabDone = true; return; }
		GrabSize = SV->GetSizeXY();
		Grabber = new FFrameGrabber(SV.ToSharedRef(), GrabSize);
		Grabber->StartCapturingFrames();
		RawWriter = IFileManager::Get().CreateFileWriter(*(FPaths::ProjectSavedDir() / TEXT("MOB") / FString::Printf(TEXT("rec_%s.bgra"), *Tag)));
		UE_LOG(LogMOB, Log, TEXT("MOB_REC_START size=%dx%d"), GrabSize.X, GrabSize.Y);
	}
	if (!Grabber) return;

	if (T <= RecStart + RecDur)
	{
		Grabber->CaptureThisFrame(FFramePayloadPtr());
		DrainGrabber();
		return;
	}

	if (Grabber->IsCapturingFrames()) Grabber->StopCapturingFrames();
	DrainGrabber();
	if (!Grabber->HasOutstandingFrames())
	{
		Grabber->Shutdown();
		delete Grabber;
		Grabber = nullptr;
		if (RawWriter) { RawWriter->Close(); delete RawWriter; RawWriter = nullptr; }
		bGrabDone = true;
		const double Span = GrabLastAt - GrabFirstAt;
		UE_LOG(LogMOB, Log, TEXT("MOB_REC size=%dx%d frames=%d seconds=%.3f fps=%.2f"),
			GrabSize.X, GrabSize.Y, GrabbedFrames, Span, Span > 0 ? (GrabbedFrames - 1) / Span : 0.0);
	}
}

void UMOBridgeSubsystem::DrainGrabber()
{
	for (FCapturedFrameData& F : Grabber->GetCapturedFrames())
	{
		if (F.BufferSize != GrabSize || !RawWriter) continue;
		RawWriter->Serialize(F.ColorBuffer.GetData(), F.ColorBuffer.Num() * sizeof(FColor));
		const double Now = FPlatformTime::Seconds();
		if (GrabbedFrames == 0) GrabFirstAt = Now;
		GrabLastAt = Now;
		++GrabbedFrames;
	}
}

// ─────────────────────────────────────────────────────────────
//  끝 — 요약을 찍고 나간다
// ─────────────────────────────────────────────────────────────

void UMOBridgeSubsystem::FinishAndQuit(const TCHAR* Why)
{
	if (bViewLog) UE_LOG(LogMOB, Log, TEXT("MOB_VIEWCLAMP on=%d frames=%lld"), bViewClamp ? 1 : 0, ViewClampFrames);
	if (bViewLog) UE_LOG(LogMOB, Log, TEXT("MOB_VIEWPITCH want=%.3f seen=%.3f"), RemotePitchWantMax, RemotePitchSeenMax);
	if (bQuitting) return;
	bQuitting = true;

	UE_LOG(LogMOB, Log, TEXT("MOB_SUMMARY why=%s player=%u team=%u spawns=%lld sent=%lld skipped=%lld fire=%lld jump=%lld reload=%lld weaponChange=%lld ack=%u maxAckLag=%d sendShort=%lld ads=%lld adsComparedOn=%lld adsFlagDiff=%lld"),
		Why, PlayerId, Team, SpawnsRecv, InputsSent, InputsSkipped, InputsFire, InputsJump, InputsReload, InputsWeaponChange, AckSeq, MaxAckLag, SendShort, InputsAds, AdsComparedOn, AdsFlagDiff);
	UE_LOG(LogMOB, Log, TEXT("MOB_SUMMARY2 states=%lld statesWithoutMe=%lld shots=%lld damages=%lld matches=%lld kills=%lld unknown=%lld hp=%.0f mag=%d spare=%d phase=%u score=%u:%u posDiff n=%lld avg=%.1f max=%.1f"),
		StatesRecv, StatesWithoutMe, ShotsRecv, DamagesRecv, MatchesRecv, KillsRecv, UnknownPackets, ServerHp, ServerMagazine, ServerSpare, MatchPhase, ScoreA, ScoreB,
		PosDiffCount, PosDiffCount ? PosDiffSum / PosDiffCount : 0.0, PosDiffMax);
	UE_LOG(LogMOB, Log, TEXT("MOB_SUMMARY3 posNonZero=%lld velMax=%.3f corrections=%lld correctionAvg=%.2f correctionMax=%.2f visualSnaps=%lld pawnChanges=%lld selfDriftMax=%.3f selfDriftOver1cm=%lld maxFrameGap=%.3f"),
		PosDiffNonZero, VelDiffMax, Corrections, Corrections ? CorrectionSum / Corrections : 0.0, CorrectionMax, VisualSnaps, PawnChanges, SelfDriftMax, SelfDriftOver1cm, MaxFrameGap);
	UE_LOG(LogMOB, Log, TEXT("MOB_CROUCH presses=%lld comparedOn=%lld flagDiff=%lld selfChanges=%lld remoteChanges=%lld remoteLiftFrames=%lld lyraBlocked=%lld"),
		InputsCrouch, CrouchComparedOn, CrouchFlagDiff, SelfCrouchChanges, RemoteCrouchChanges, RemoteLiftFrames, GLyraCrouchBlocked);
	// 내 몸 Quinn(D85) — donePawns = 몸 Quinn · 여성형 층으로 닫은 내 캐릭터 수(부활마다 하나). sets = 몸 바꾸기 부른 수 · reequips = 층 때문에 다시 쥔 수
	UE_LOG(LogMOB, Log, TEXT("MOB_SELFBODY_SUMMARY donePawns=%lld sets=%lld reequips=%lld fails=%lld selfRespawns=%lld emptyFrames=%lld lastDone=%d delay=%.1f"),
		SelfBodyDonePawns, SelfBodySets, SelfReequips, SelfBodyFails, (int64)SelfRespawns, SelfBodyEmptyFrames, bSelfBodyDone ? 1 : 0, SelfBodyDelay);
	// 사람끼리 막기(D83) — 내 예측이 남 캡슐에 닿은 수. 0 이면 보정이 적어도 막기 갈래가 안 돈 것이다(봇 BOT pawn predHits 와 같은 계수)
	if (Core)
	{
		const mo::MoveCounters& MCount = Core->Sim.GetCounters();
		UE_LOG(LogMOB, Log, TEXT("MOB_PAWN predHits=%llu predStartPen=%llu predOdd=%llu predTouch=%llu pawnsLast=%d corrections=%lld"),
			(unsigned long long)MCount.pawnHits, (unsigned long long)MCount.pawnStartPenetrating, (unsigned long long)MCount.pawnOddSimplex, (unsigned long long)MCount.pawnTouchStart,
			(int32)Core->Pawns.size(), Corrections);
	}
	if (Core)
	{
		const mo::CSelfPrediction::MeleeCounters& MC = Core->Prediction.GetMeleeCounters();
		UE_LOG(LogMOB, Log, TEXT("MOB_MELEE presses=%lld judged=%lld screen=%lld/%lld/%lld/%lld mine=%lld/%lld/%lld/%lld kindDiff=%lld targetDiff=%lld noGuess=%lld forceDiffMax=%.3f others=%lld"),
			InputsMelee, MeleeJudged, MeleeScreen[0], MeleeScreen[1], MeleeScreen[2], MeleeScreen[3], MeleeMine[0], MeleeMine[1], MeleeMine[2], MeleeMine[3], MeleeKindDiff, MeleeTargetDiff, MeleeNoGuess, MeleeForceDiffMax, MeleeOthers);
		UE_LOG(LogMOB, Log, TEXT("MOB_MELEE2 started=%llu results=%llu resultsNoSlot=%llu startMismatches=%llu pushesGiven=%llu pushesIgnored=%llu hitFixes=%llu forceFixes=%llu unconfirmed=%llu rerunsMissed=%llu"),
			(unsigned long long)MC.started, (unsigned long long)MC.results, (unsigned long long)MC.resultsNoSlot, (unsigned long long)MC.startMismatches, (unsigned long long)MC.pushesGiven,
			(unsigned long long)MC.pushesIgnored, (unsigned long long)MC.pushHitFixes, (unsigned long long)MC.pushForceFixes, (unsigned long long)MC.unconfirmed, (unsigned long long)MC.rerunsMissed);
		UE_LOG(LogMOB, Log, TEXT("MOB_MELEE3 queued=%lld driven=%lld activateFail=%lld noSpec=%lld dropped=%lld waitFrames=%lld lyraSelf=%lld stripped=%lld"),
			MeleeQueued, MeleeDriven, MeleeActivateFail, MeleeNoSpec, MeleeDropped, MeleeWaitFrames, LyraMeleeSelf, LyraMeleeStripped);
	}
	UE_LOG(LogMOB, Log, TEXT("MOB_SUMMARY4 remotes=%d remoteFail=%d frames=%lld underrun=%lld teleports=%lld driftMax=%.3f driftOver1cm=%lld yawDiffMax=%.3f lagAvg=%.1f behindTicksAvg=%.2f"),
		RemoteSpawned, RemoteSpawnFail, RemoteFrames, RemoteUnderrun, RemoteTeleports, RemoteDriftMax, RemoteDriftOver1cm, RemoteYawDiffMax, RemoteFrames ? RemoteLagSum / RemoteFrames : 0.0, RemoteFrames ? RemoteBehindSum / RemoteFrames : 0.0);

	if (bServerAnim)
	{
		UE_LOG(LogMOB, Log, TEXT("MOB_SERVERANIM_SUMMARY tailPlayers=%lld tailBad=%lld noState=%lld poseEvals=%lld poseFrames=%lld poseFail=%lld"), AnimTailPlayers, AnimTailBad, AnimNoState, AnimPoseEvals, AnimPoseFrames, AnimPoseFail);
		// 무기별(9단계) — 무기별 자세 계산 수의 합 = poseEvals. setChanges = 언리얼 경로 클립 한 벌을 갈아 넣은 수, setMisses = 그 무기 한 벌이 없던 프레임, badWeapon = 모르는 무기 번호로 받은 칸
		// adsLayers = 언리얼 경로에서 조준 전환 층을 허리 전환으로 바꿔 튼 수, badLayers = 칸 밖이라 버린 층(0 이어야 함, C-5)
		// crouchLayers = 앉은 전환 층을 허리 전환으로 바꿔 튼 수, stanceLayers = 서기 안 앉기 · 일어서기 전환 층을 걷기 · 서기 자리로 돌린 수(D-3)
		UE_LOG(LogMOB, Log, TEXT("MOB_SERVERANIM_WEAPON poseEvals pistol=%lld rifle=%lld shotgun=%lld setChanges=%lld setMisses=%lld badWeapon=%lld adsLayers=%lld badLayers=%lld crouchLayers=%lld stanceLayers=%lld"),
			AnimPoseEvalsByWeapon[0], AnimPoseEvalsByWeapon[1], AnimPoseEvalsByWeapon[2], AnimSetChanges, AnimSetMisses, AnimBadWeapon, AnimAdsLayers, AnimBadLayers, AnimCrouchLayers, AnimStanceLayers);
		// 몽타주(9단계 5-a) — cellsIn = 받은 칸 수의 합(프레임마다), cells = 언리얼 경로에 넣은 칸, misses = 무기 · 자산이 없어 못 넣은 칸(cellsIn = cells + misses).
		// frames = 칸이 하나라도 든 프레임, noSlot = 칸을 넣었는데 끼움 자리를 하나도 안 거친 프레임, maskMissing = 윗몸 뼈 가림을 못 찾은 프레임.
		// weaponNotifies = 튼 무기 메시 몽타주 알림(남의 사격 · 재장전 시작마다 하나), weaponPlaying = 그중 튼 직후 무기 메시에 몽타주가 돌던 수, soundNotifies = 튼 소리 알림
		UE_LOG(LogMOB, Log, TEXT("MOB_SERVERANIM_MONTAGE cellsIn=%lld cells=%lld misses=%lld frames=%lld slotPasses=%lld noSlot=%lld maskMissing=%lld weaponNotifies=%lld weaponPlaying=%lld soundNotifies=%lld restarts=%lld stepStates=%lld"),
			AnimMontageCellsIn, AnimMontageCells, AnimMontageMisses, AnimMontageFrames, AnimSlotPasses, AnimMontageNoSlot, AnimMaskMissing, AnimWeaponNotifies, AnimWeaponPlaying, AnimSoundNotifies, AnimMontageRestarts, AnimStepStates);
		if (bRayCheck)
		{
			UE_LOG(LogMOB, Log, TEXT("MOB_RAYCHECK_NOBODY count=%lld"), RayNoBody);
			for (int32 RadiusIndex = 0; RadiusIndex < 2; ++RadiusIndex)
			{
				for (int32 AimHead = 0; AimHead < 2; ++AimHead)
				{
					const FMOBRayStats& S = RayStats[RadiusIndex][AimHead];
					UE_LOG(LogMOB, Log, TEXT("MOB_RAYCHECK r=%.1f aimHead=%d rays=%lld map=%lld engHit=%lld same=%lld engOnly=%lld mineOnly=%lld both=%lld boneSame=%lld engWeak=%lld weakLost=%lld weakGain=%lld"), RadiusIndex ? 5.5 : 0.0, AimHead, S.Rays, S.Map, S.EngHit, S.Same, S.EngOnly, S.MineOnly, S.Both, S.BoneSame, S.EngWeak, S.WeakLost, S.WeakGain);
				}
			}
		}
	}
	UE_LOG(LogMOB, Log, TEXT("MOB_PICKUPS hidden=%d hpFixes=%lld"), PickupsHidden, HpFixes);
	UE_LOG(LogMOB, Log, TEXT("MOB_PADS pads=%d msgs=%lld takes=%lld readies=%lld syncs=%lld badIndex=%lld noActor=%lld bound=%lld heals=%lld healBeforeFix=%lld healMismatch=%lld healNoPawn=%lld stateChecks=%lld stateDiff=%lld lastStateTick=%u"),
		Pads.Num(), PadMsgs, PadTakes, PadReadies, PadSyncs, PadBadIndex, PadNoActor, PadBound, PadHeals, PadHealBeforeFix, PadHealMismatch, PadHealNoPawn, PadStateChecks, PadStateDiff, LastStateTick);
	if (!bAbilityOwnTimer)
	{
		// 내 사격 능력 구동(D53) — 칸 뜻은 MOBridgeSubsystem.h 의 FireQueued 주석. pending = 끝날 때 아직 쌓여 있던 발
		UE_LOG(LogMOB, Log, TEXT("MOB_ABILITYFIRE queued=%lld driven=%lld activateFail=%lld noSpec=%lld dropped=%lld pending=%d waitFrames=%lld weaponMismatch=%lld gateBlocks=%lld gateDryFire=%lld bullets=%lld bulletsOutside=%lld lag=%lld,%lld,%lld,%lld lyraAimMaxDeg=%.4f startDiffMax=%.2f"),
			FireQueued, FireDriven, FireActivateFail, FireNoSpec, FireDropped, PendingFires.Num(), FireWaitFrames, FireWeaponMismatch, FireGateBlocks, FireGateDryFire, FireBullets, FireBulletsOutside,
			FireLagFrames[0], FireLagFrames[1], FireLagFrames[2], FireLagFrames[3], FireLyraAimMaxDeg, FireStartDiffMax);
	}
	if (bAnimLog)
	{
		FString States;
		for (const TPair<FName, int64>& P : AnimStates) States += FString::Printf(TEXT("%s=%lld "), *P.Key.ToString(), P.Value);
		UE_LOG(LogMOB, Log, TEXT("MOB_ANIM_SUM moving=%lld rendered=%lld noInput=%lld noAccel=%lld states=[%s]"), AnimMoving, AnimMovingRendered, AnimMovingNoInput, AnimMovingNoAccel, *States);
	}
	UE_LOG(LogMOB, Log, TEXT("MOB_SUMMARY6 inputRawMax=%.3f inputSentMax=%.6f diagonal=%lld bulletsLyra=%lld bulletsServer=%lld bulletsLyraImmune=%lld bulletsServerImmune=%lld bulletsLyraOther=%lld bulletsServerOther=%lld magDiffMax=%d myShots=%lld,%lld,%lld reloadStart=%lld,%lld,%lld reloadDone=%lld,%lld,%lld aimFarLocal=%lld aimDistMax=%.1f aimHorizMax=%.1f"),
		InputRawMax, InputSentMax, InputsDiagonal, BulletsLyra, BulletsServer, BulletsLyraImmune, BulletsServerImmune, BulletsLyraOther, BulletsServerOther, MagDiffMax, MyShots[0], MyShots[1], MyShots[2], ReloadStarts[0], ReloadStarts[1], ReloadStarts[2], ReloadDones[0], ReloadDones[1], ReloadDones[2], AimFarLocal, AimDistMax, AimHorizMax);
	for (int32 Slot = 0; Slot < 4; ++Slot)
	{
		int64 Inputs = 0;
		double DistMax = 0, HorizMax = 0;
		FString Bins;
		for (int32 B = 0; B < 18; ++B)
		{
			if (AimSweepInputs[Slot][B] == 0) continue;
			Inputs += AimSweepInputs[Slot][B];
			DistMax = FMath::Max(DistMax, AimSweepDistMax[Slot][B]);
			HorizMax = FMath::Max(HorizMax, AimSweepHorizMax[Slot][B]);
			Bins += FString::Printf(TEXT("%d:%lld:%.1f/%.1f "), B * 10 - 90, AimSweepInputs[Slot][B], AimSweepDistMax[Slot][B], AimSweepHorizMax[Slot][B]);
		}
		UE_LOG(LogMOB, Log, TEXT("MOB_AIMSWEEP ads=%d crouch=%d inputs=%lld skipped=%lld pitch=%.1f~%.1f distMax=%.1f horizMax=%.1f bins(pitch:n:dist/horiz)=[%s]"), Slot % 2, Slot / 2, Inputs, AimSweepSkipped, AimSweepPitchMin, AimSweepPitchMax, DistMax, HorizMax, *Bins);
	}
	// 선 직후 · 앉은 직후 0.25초(입력:거리 최대)와 그 뒤 — 앉은 직후는 카메라가 아직 선 높이라 거리가 늘고, 선 직후는 아직 앉은 높이라 줄어야 한다(D-6-a)
	UE_LOG(LogMOB, Log, TEXT("MOB_AIMFRESH standFresh=%lld:%.1f standSteady=%.1f crouchFresh=%lld:%.1f crouchSteady=%.1f"),
		AimSweepFreshInputs[0], AimSweepFreshMax[0], AimSweepSteadyMax[0], AimSweepFreshInputs[1], AimSweepFreshMax[1], AimSweepSteadyMax[1]);
	// 조준 키 → Lyra 최대 속도 늦음(D61). lag0 = 같은 프레임 · none = 3프레임 안에 안 바뀜
	UE_LOG(LogMOB, Log, TEXT("MOB_ADSKEY_SUMMARY press lag0=%lld lag1=%lld lag2=%lld none=%lld | release lag0=%lld lag1=%lld lag2=%lld none=%lld"),
		AdsKeyLag[1][0], AdsKeyLag[1][1], AdsKeyLag[1][2], AdsKeyLag[1][3], AdsKeyLag[0][0], AdsKeyLag[0][1], AdsKeyLag[0][2], AdsKeyLag[0][3]);
	UE_LOG(LogMOB, Log, TEXT("MOB_SUMMARY5 damageApplied=%lld damageMismatch=%lld damageNoTarget=%lld hpCompared=%lld hpDiffCount=%lld hpDiffMax=%.3f selfRespawns=%lld remoteRespawns=%lld scoreFixes=%lld scoreServer=%u:%u scoreLyra=%d:%d ammoFixes=%lld weaponSwaps=%lld remoteEquips=%lld phaseChanges=%lld shotCues=%lld hitMarkers=%lld countDownSets=%lld countDownLast=%d"),
		DamageApplied, DamageMismatch, DamageNoTarget, HpCompared, HpDiffCount, HpDiffMax, SelfRespawns, RemoteRespawns, ScoreFixes, ScoreA, ScoreB,
		LyraScoreA, LyraScoreB,
		AmmoFixes, WeaponSwaps, RemoteEquips, PhaseChanges, ShotCues, HitMarkers, CountDownSets, LastCountDownShown);

	FPlatformMisc::RequestExit(false, TEXT("MOBridge"));
}

// ─────────────────────────────────────────────────────────────
//  도우미
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────
//  서버 결과를 Lyra 화면에 — 값은 전부 서버 것이고, Lyra 는 체력 바·죽는 모습·알림·점수판을 그대로 보여 준다
// ─────────────────────────────────────────────

APawn* UMOBridgeSubsystem::FindPlayerPawn(uint32 Id) const
{
	if (Id == 0) return nullptr;
	if (Id == PlayerId)
	{
		const APlayerController* PC = GetWorld()->GetFirstPlayerController();
		return PC ? PC->GetPawn() : nullptr;
	}
	const FMOBRemote* R = Remotes.Find(Id);
	return R ? R->Pawn.Get() : nullptr;
}

APlayerState* UMOBridgeSubsystem::FindPlayerState(uint32 Id) const
{
	if (Id == 0) return nullptr;
	if (Id == PlayerId)
	{
		const APlayerController* PC = GetWorld()->GetFirstPlayerController();
		return PC ? PC->PlayerState : nullptr;
	}
	const FMOBRemote* R = Remotes.Find(Id);
	return R && R->Ctrl.IsValid() ? R->Ctrl->PlayerState : nullptr;
}

int32 UMOBridgeSubsystem::LyraTeamOf(uint8 ServerTeam) const
{
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const IGenericTeamAgentInterface* LocalTA = PC ? Cast<IGenericTeamAgentInterface>(PC->PlayerState) : nullptr;
	const int32 LocalLyraTeam = LocalTA ? LocalTA->GetGenericTeamId().GetId() : 1;
	return ServerTeam == Team ? LocalLyraTeam : (LocalLyraTeam == 1 ? 2 : 1);
}

// 받은 양이 아니라 서버 체력에 맞춘다 — 한 번 어긋나도 다음 피해에서 닫히게
void UMOBridgeSubsystem::ApplyServerHp(uint32 TargetId, uint32 AttackerId, float HpAfter)
{
	APawn* Target = FindPlayerPawn(TargetId);
	UAbilitySystemComponent* TargetASC = FindASC(Target);
	ULyraHealthComponent* HC = ULyraHealthComponent::FindHealthComponent(Target);
	if (!TargetASC || !HC)
	{
		++DamageNoTarget;
		return;
	}
	if (HC->IsDeadOrDying()) return;
	const float Amount = HC->GetHealth() - HpAfter;
	if (Amount <= 0.f) return;

	// 무적은 서버 판정이다 — 서버가 피해를 줬으면 Lyra 쪽 무적(시작 효과 등)이 막으면 안 된다
	if (TargetASC->HasMatchingGameplayTag(TAG_Gameplay_DamageImmunity))
	{
		TargetASC->RemoveActiveEffectsWithGrantedTags(FGameplayTagContainer(TAG_Gameplay_DamageImmunity));
	}

	APawn* Attacker = FindPlayerPawn(AttackerId);
	UAbilitySystemComponent* SourceASC = FindASC(Attacker);
	if (!SourceASC) SourceASC = TargetASC;

	FGameplayCueParameters Cue;
	Cue.RawMagnitude = Amount;
	Cue.NormalizedMagnitude = Amount;
	Cue.Location = Target->GetActorLocation();
	Cue.Normal = FVector::UpVector;
	Cue.Instigator = Attacker ? Attacker : Target;
	Cue.EffectCauser = Attacker ? Attacker : Target;
	Cue.EffectContext = SourceASC->MakeEffectContext();

	// 처치 알림·점수 규칙은 공격자 쪽 맥락(플레이어 상태 + 캐릭터)을 읽는다 — Lyra 무기 효과와 같은 모양
	FGameplayEffectSpecHandle Spec = SourceASC->MakeOutgoingSpec(UMOBServerDamageEffect::StaticClass(), 1.f, SourceASC->MakeEffectContext());
	Spec.Data->SetSetByCallerMagnitude(LyraGameplayTags::SetByCaller_Damage, Amount);
	SourceASC->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), TargetASC);
	++DamageApplied;
	if (!FMath::IsNearlyEqual(HC->GetHealth(), HpAfter, 0.01f))
	{
		++DamageMismatch;
		UE_LOG(LogMOB, Warning, TEXT("MOB_DAMAGE_MISMATCH target=%u lyra=%.2f server=%.2f immune=%d"), TargetId, HC->GetHealth(), HpAfter,
			TargetASC->HasMatchingGameplayTag(TAG_Gameplay_DamageImmunity) ? 1 : 0);
	}

	static const FGameplayTag DamageCue = FGameplayTag::RequestGameplayTag(TEXT("GameplayCue.Character.DamageTaken"), false);
	if (DamageCue.IsValid()) TargetASC->ExecuteGameplayCue(DamageCue, Cue);
}

void UMOBridgeSubsystem::CompareHp(const AActor* Actor, float ServerValue)
{
	const ULyraHealthComponent* HC = ULyraHealthComponent::FindHealthComponent(Actor);
	if (!HC || HC->IsDeadOrDying() || HC->GetMaxHealth() <= 0.f) return;
	const double Diff = FMath::Abs(HC->GetHealth() - ServerValue);
	++HpCompared;
	HpDiffMax = FMath::Max(HpDiffMax, Diff);
	if (Diff > 0.01)
	{
		++HpDiffCount;
		if (HpDiffLinesLeft > 0)
		{
			--HpDiffLinesLeft;
			double NearestSpawner = -1;
			for (TActorIterator<ALyraWeaponSpawner> It(GetWorld()); It; ++It)
			{
				const double D = FVector::Dist(It->GetActorLocation(), Actor->GetActorLocation());
				if (NearestSpawner < 0 || D < NearestSpawner) NearestSpawner = D;
			}
			UE_LOG(LogMOB, Warning, TEXT("MOB_HP_DIFF pawn=%s lyra=%.2f server=%.2f nearestPickup=%.0f"), *GetNameSafe(Actor), HC->GetHealth(), ServerValue, NearestSpawner);
		}
		// 체력은 서버 값이다 — 까닭이 무엇이든 다음 상태에서 닫는다. 0 은 죽음 흐름(처치 패킷)이 맡는다
		if (ServerValue > 0.f)
		{
			if (UAbilitySystemComponent* ASC = FindASC(const_cast<AActor*>(Actor)))
			{
				ASC->SetNumericAttributeBase(ULyraHealthSet::GetHealthAttribute(), ServerValue);
				++HpFixes;
			}
		}
	}
}

// 맵의 줍기 받침 — Lyra 혼자 체력·무기를 주면 서버와 어긋난다. 서버 받침(체력, B-3)은 묶어 서버 소식으로 그리고, 나머지(무기 받침)는 서버에 줍기가 생기기 전까지 숨긴다(월드 파티션이 나중에 올리는 것도 있어 계속 본다)
void UMOBridgeSubsystem::TickPickups(double Now)
{
	if (!bNamedSelf && PlayerId != 0)
	{
		// 기본 이름은 PC 이름이다 — 처치 알림에 그대로 보인다
		const APlayerController* PC = GetWorld()->GetFirstPlayerController();
		if (PC && PC->PlayerState)
		{
			PC->PlayerState->SetPlayerName(FString::Printf(TEXT("Player%u"), PlayerId));
			bNamedSelf = true;
		}
	}
	if (bKeepPickups) return;

	// 대기 표시 — Lyra Tick 은 자기 대기 타이머가 돌 때만 CoolDownPercentage 를 쓴다. 다리는 그 타이머를 안 거니 같은 식(1 - 남은 시간 / CoolDownTime)을 서버 틱으로 쓴다
	if (bHasTickClock)
	{
		static FFloatProperty* PercentProp = FindFProperty<FFloatProperty>(ALyraWeaponSpawner::StaticClass(), TEXT("CoolDownPercentage"));
		const double ServerTickNow = Now * wire::kTickRate + TickClockOffset;
		for (const FMOBPad& Pad : Pads)
		{
			ALyraWeaponSpawner* A = Pad.Actor.Get();
			if (!A || !PercentProp || Pad.bAvailable || Pad.CooldownTicks == 0) continue;
			PercentProp->SetPropertyValue_InContainer(A, (float)FMath::Clamp(1.0 - ((double)Pad.ReadyTick - ServerTickNow) / Pad.CooldownTicks, 0.0, 1.0));
		}
	}

	// 묶인 받침 액터가 없어졌으면(경기 시작에 Lyra 가 받침을 새로 놓는다 — 0-3 pads1 · 월드 파티션이 늦게 올림) 이 프레임에 다시 찾는다.
	// 새 액터는 충돌이 켜져 있어, 1초를 기다리면 그사이 Lyra 가 혼자 집을 수 있다
	bool bLost = false;
	for (const FMOBPad& Pad : Pads) bLost |= Pad.bKnown && !Pad.Actor.IsValid();
	if (!bLost && Now < NextPickupCheckAt) return;
	NextPickupCheckAt = Now + 1.0;
	for (int32 i = 0; i < Pads.Num(); ++i)
	{
		if (Pads[i].bKnown && !Pads[i].Actor.IsValid()) BindPad(i);
	}
	for (TActorIterator<ALyraWeaponSpawner> It(GetWorld()); It; ++It)
	{
		bool bBound = false;
		for (const FMOBPad& Pad : Pads) bBound |= Pad.Actor.Get() == *It;
		if (bBound || It->IsHidden()) continue;
		// 서버 받침이 아닌 것(무기 받침 · 서버를 받침 없이 띄운 판) — 서버에 없는 줍기라 숨긴다
		It->SetActorEnableCollision(false);
		It->SetActorHiddenInGame(true);
		It->SetActorTickEnabled(false);
		++PickupsHidden;
	}

	// 1초마다 — 묶인 받침의 Lyra 상태가 서버 상태와 같은가. Lyra 자기 겹침 · 타이머 · 새로 놓기가 끼어들면 여기서 드러난다
	static FBoolProperty* AvailableProp = FindFProperty<FBoolProperty>(ALyraWeaponSpawner::StaticClass(), TEXT("bIsWeaponAvailable"));
	for (int32 i = 0; i < Pads.Num(); ++i)
	{
		ALyraWeaponSpawner* A = Pads[i].Actor.Get();
		if (!A || !AvailableProp) continue;
		++PadStateChecks;
		const bool bLyraAvailable = AvailableProp->GetPropertyValue_InContainer(A);
		const bool bShown = A->WeaponMesh && A->WeaponMesh->IsVisible();
		const bool bCollision = A->GetActorEnableCollision();
		if (bLyraAvailable == Pads[i].bAvailable && bShown == Pads[i].bAvailable && !bCollision) continue;
		++PadStateDiff;
		if (PadDiffLinesLeft > 0)
		{
			--PadDiffLinesLeft;
			UE_LOG(LogMOB, Warning, TEXT("MOB_PAD_DIFF pad=%d server=%d lyra=%d shown=%d collision=%d"), i, Pads[i].bAvailable ? 1 : 0, bLyraAvailable ? 1 : 0, bShown ? 1 : 0, bCollision ? 1 : 0);
		}
		A->SetActorEnableCollision(false);
		ApplyPadLook(*A, Pads[i].bAvailable, false);
	}
}

// 체력 받침(B-3) — 서버가 정한 상태를 맵의 받침 액터에 그린다. Lyra 자기 겹침 · 대기 타이머는 쓰지 않는다(묶을 때 충돌을 끔).
// 집음이면 집은 사람에게 받침 자기 회복을 건다(B_AbilitySpawner GiveWeapon 그래프 = 효과 GameplayEffectToApply + 줍기 신호, content0 pad_flow.txt:10-54)
void UMOBridgeSubsystem::HandlePickup(const wire::Pickup& M)
{
	if (bBreakPadDrop)
	{
		bBreakPadDrop = false;
		UE_LOG(LogMOB, Log, TEXT("MOB_BREAK_PADS dropped tick=%u pad=%u reason=%u"), M.tick, M.pad, M.reason);
		return;
	}
	++PadMsgs;
	const bool bTake = M.reason == wire::PICKUP_TAKE || M.reason == wire::PICKUP_TAKE_RECHECK;
	if (bTake) ++PadTakes;
	else if (M.reason == wire::PICKUP_READY) ++PadReadies;
	else ++PadSyncs;
	if (Pads.Num() < M.padCount) Pads.SetNum(M.padCount);
	if (M.pad >= Pads.Num())
	{
		++PadBadIndex;
		return;
	}
	FMOBPad& Pad = Pads[M.pad];
	Pad.bKnown = true;
	Pad.bAvailable = M.available != 0;
	Pad.ReadyTick = M.readyTick;
	Pad.CooldownTicks = M.cooldownTicks;
	Pad.Location = FVector(M.x, M.y, M.z);
	ALyraWeaponSpawner* Actor = bKeepPickups ? nullptr : Pad.Actor.Get();
	if (!Actor && !bKeepPickups) Actor = BindPad(M.pad);	// 묶을 때 지금 상태로 그린다
	if (!Actor) ++PadNoActor;
	// 효과는 집음 · 다시 켜짐만 — 현황(들어올 때 · 경기 시작)은 상태만 맞춘다. Lyra 도 새로 놓은 받침은 효과 없이 켜진다
	if (Actor) ApplyPadLook(*Actor, Pad.bAvailable, M.reason != wire::PICKUP_SYNC);
	const float LyraHp = bTake && !bKeepPickups ? ApplyPadHeal(M, Actor) : -1.f;
	static const TCHAR* ReasonNames[] = { TEXT("take"), TEXT("takeRecheck"), TEXT("ready"), TEXT("sync") };
	UE_LOG(LogMOB, Log, TEXT("MOB_PAD tick=%u pad=%u reason=%s avail=%u ready=%u picker=%u heal=%.1f hpAfter=%.1f lyraHp=%.1f actor=%d"), M.tick, M.pad,
		M.reason < UE_ARRAY_COUNT(ReasonNames) ? ReasonNames[M.reason] : TEXT("?"), M.available, M.readyTick, M.picker, M.heal, M.hpAfter, LyraHp, Actor ? 1 : 0);
}

// 서버 받침 자리에 있는 맵 받침을 찾아 묶는다. 받침 파일 자리는 make_pads.py 가 맵 액터 자리와 0.05 안인 것만 낸다 — 5 는 받침 사이(1086 이상)에 견주면 0 이다
ALyraWeaponSpawner* UMOBridgeSubsystem::BindPad(int32 Index)
{
	FMOBPad& Pad = Pads[Index];
	if (!Pad.bKnown) return nullptr;
	constexpr double kBindReach = 5.0;
	for (TActorIterator<ALyraWeaponSpawner> It(GetWorld()); It; ++It)
	{
		if (FVector::Dist(It->GetActorLocation(), Pad.Location) > kBindReach) continue;
		bool bTaken = false;
		for (const FMOBPad& Other : Pads) bTaken |= Other.Actor.Get() == *It;
		if (bTaken) continue;
		ALyraWeaponSpawner* A = *It;
		A->SetActorEnableCollision(false);	// Lyra 자기 겹침 집기를 끈다 — 집기는 서버 몫
		A->SetActorHiddenInGame(false);		// 서버 소식 전에 숨긴 판이면 다시 보이게
		A->SetActorTickEnabled(true);		// 구슬 돌기. Lyra Tick 은 자기 대기 타이머가 없으면 대기 표시를 안 건드린다
		Pad.Actor = A;
		++PadBound;
		ApplyPadLook(*A, Pad.bAvailable, false);
		UE_LOG(LogMOB, Log, TEXT("MOB_PAD_BIND pad=%d actor=%s at=(%.1f,%.1f,%.1f) avail=%d"), Index, *GetNameSafe(A), A->GetActorLocation().X, A->GetActorLocation().Y, A->GetActorLocation().Z, Pad.bAvailable ? 1 : 0);
		return A;
	}
	return nullptr;
}

// Lyra 가 집음(AttemptPickUpWeapon) · 다시 켜짐(ResetCoolDown)에서 하는 화면 쪽만 — 켜짐 값 · 구슬 보임 · 효과. 대기 타이머 · 다시 검사 타이머는 안 건다
void UMOBridgeSubsystem::ApplyPadLook(ALyraWeaponSpawner& Actor, bool bAvailable, bool bEffects)
{
	static FBoolProperty* AvailableProp = FindFProperty<FBoolProperty>(ALyraWeaponSpawner::StaticClass(), TEXT("bIsWeaponAvailable"));
	static FFloatProperty* PercentProp = FindFProperty<FFloatProperty>(ALyraWeaponSpawner::StaticClass(), TEXT("CoolDownPercentage"));
	if (AvailableProp) AvailableProp->SetPropertyValue_InContainer(&Actor, bAvailable);
	Actor.SetWeaponPickupVisibility(bAvailable);
	if (bAvailable && PercentProp) PercentProp->SetPropertyValue_InContainer(&Actor, 0.f);
	if (!bEffects) return;
	if (bAvailable) Actor.PlayRespawnEffects();
	else Actor.PlayPickupEffects();
}

// 회복 — 받침 자기 그래프(GiveWeapon)를 그대로 부른다: 체력 < 최대면 받침 효과를 걸고 줍기 신호를 낸다. 체력은 서버 값이라 앞뒤를 서버 값과 견준다.
// 돌려주는 값 = 끝난 뒤 Lyra 체력(-1 = 몸 없음)
float UMOBridgeSubsystem::ApplyPadHeal(const wire::Pickup& M, ALyraWeaponSpawner* Actor)
{
	APawn* Pawn = FindPlayerPawn(M.picker);
	ULyraHealthComponent* HC = ULyraHealthComponent::FindHealthComponent(Pawn);
	UAbilitySystemComponent* ASC = FindASC(Pawn);
	if (!Pawn || !HC || !ASC || HC->IsDeadOrDying())
	{
		++PadHealNoPawn;
		return -1.f;
	}
	// 회복 전 체력이 서버와 다르면(시험 순간이동이 체력을 바꾼 바로 그 틱에 집은 때 등) 먼저 맞춘다 — 회복 효과 하나만 따로 보려고
	const float Before = M.hpAfter - M.heal;
	if (!FMath::IsNearlyEqual(HC->GetHealth(), Before, 0.01f))
	{
		++PadHealBeforeFix;
		UE_LOG(LogMOB, Log, TEXT("MOB_PAD_HEAL_BEFORE tick=%u pad=%u picker=%u lyra=%.2f server=%.2f"), M.tick, M.pad, M.picker, HC->GetHealth(), Before);
		ASC->SetNumericAttributeBase(ULyraHealthSet::GetHealthAttribute(), Before);
	}
	if (bBreakPadHeal)
	{
		bBreakPadHeal = false;
		UE_LOG(LogMOB, Log, TEXT("MOB_BREAK_PADS skippedHeal tick=%u pad=%u picker=%u"), M.tick, M.pad, M.picker);
	}
	else if (Actor)
	{
		Actor->GiveWeapon(TSubclassOf<ULyraInventoryItemDefinition>(), Pawn);
		++PadHeals;
	}
	if (!FMath::IsNearlyEqual(HC->GetHealth(), M.hpAfter, 0.01f))
	{
		++PadHealMismatch;
		UE_LOG(LogMOB, Warning, TEXT("MOB_PAD_HEAL_DIFF tick=%u pad=%u picker=%u lyra=%.2f server=%.2f before=%.2f actor=%d"), M.tick, M.pad, M.picker, HC->GetHealth(), M.hpAfter, Before, Actor ? 1 : 0);
		ASC->SetNumericAttributeBase(ULyraHealthSet::GetHealthAttribute(), M.hpAfter);
	}
	return HC->GetHealth();
}

void UMOBridgeSubsystem::HandleKill(const wire::Kill& M)
{
	++KillsRecv;
	if (M.target == PlayerId && Core) Core->Prediction.BeginWaitSpawn();
	if (M.target == PlayerId)
	{
		RequestEventShot(TEXT("dead"), 0.6, 2);
		RequestEventShot(TEXT("deadlate"), 3.0, 2);
	}
	if (M.attacker == PlayerId)
	{
		RequestEventShot(TEXT("kill"), 0.15, 3);
		RequestEventShot(TEXT("killlate"), 1.0, 3);
	}
	if (FMOBRemote* R = Remotes.Find(M.target))
	{
		R->bKilled = true;
		R->bRespawnPending = false;
	}
	// 떨어져 죽으면 피해 패킷 없이 처치만 온다 — 체력 0 이 Lyra 죽음 흐름(모습·처치 알림)을 부른다
	ApplyServerHp(M.target, M.attacker, 0.f);

	ScoreA = M.teamScoreA;
	ScoreB = M.teamScoreB;
	SyncTeamScores();

	// 개인 기록 — Lyra 규칙이 이미 올렸어도 서버 값에 맞춘다
	static const FGameplayTag ElimTag = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.Score.Eliminations"));
	static const FGameplayTag DeathTag = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.Score.Deaths"));
	const auto SetStat = [this](APlayerState* PS, const FGameplayTag& Stat, int32 Want)
	{
		ALyraPlayerState* LPS = Cast<ALyraPlayerState>(PS);
		if (!LPS) return;
		const int32 Have = LPS->GetStatTagStackCount(Stat);
		if (Have == Want) return;
		++ScoreFixes;
		if (Have < Want) LPS->AddStatTagStack(Stat, Want - Have);
		else LPS->RemoveStatTagStack(Stat, Have - Want);
	};
	if (M.attacker != 0) SetStat(FindPlayerState(M.attacker), ElimTag, M.attackerKills);
	SetStat(FindPlayerState(M.target), DeathTag, M.targetDeaths);

	// 도움 — 서버가 보낸 번호만큼 센다(떨어져 죽으면 처치자가 없어 죽은 사람 빼고 전원). Lyra 가 혼자 준 도움도 여기서 서버 값으로 돌린다
	const int32 AssistCount = FMath::Min<int32>(M.assistCount, wire::kMaxPlayersInRoom - 1);
	for (int32 i = 0; i < AssistCount; ++i) ++ServerAssists.FindOrAdd(M.assisters[i]);
	AssistsRecv += AssistCount;
	SyncAssists();

	UE_LOG(LogMOB, Log, TEXT("MOB_KILL tick=%u attacker=%u target=%u weapon=%u weak=%u score=%u:%u kills=%u deaths=%u"),
		M.tick, M.attacker, M.target, M.weapon, M.weakSpot, M.teamScoreA, M.teamScoreB, M.attackerKills, M.targetDeaths);
}

// 점수판 도움을 처치 패킷으로 센 값에 맞춘다 — 나 · 남 모두. 센 적 없는 사람은 0. 고친 횟수 = Lyra 가 혼자 센 값과 달랐던 횟수(기록만)
void UMOBridgeSubsystem::SyncAssists()
{
	static const FGameplayTag AssistTag = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.Score.Assists"));
	const auto Sync = [this](uint32 Id)
	{
		ALyraPlayerState* LPS = Cast<ALyraPlayerState>(FindPlayerState(Id));
		if (!LPS) return;
		const int32* Found = ServerAssists.Find(Id);
		const int32 Want = Found ? *Found : 0;
		const int32 Have = LPS->GetStatTagStackCount(AssistTag);
		if (Have == Want) return;
		++AssistFixes;
		if (Have < Want) LPS->AddStatTagStack(AssistTag, Want - Have);
		else LPS->RemoveStatTagStack(AssistTag, Have - Want);
	};
	Sync(PlayerId);
	for (const TPair<uint32, FMOBRemote>& R : Remotes) Sync(R.Key);
}

void UMOBridgeSubsystem::SyncTeamScores()
{
	ULyraTeamSubsystem* Teams = GetWorld()->GetSubsystem<ULyraTeamSubsystem>();
	if (!Teams) return;
	static const FGameplayTag ElimTag = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.Score.Eliminations"));
	const auto Sync = [&](uint8 ServerTeam, int32 Want)
	{
		const int32 LyraTeam = LyraTeamOf(ServerTeam);
		const int32 Have = Teams->GetTeamTagStackCount(LyraTeam, ElimTag);
		if (Have == Want) return;
		++ScoreFixes;
		if (Have < Want) Teams->AddTeamTagStack(LyraTeam, ElimTag, Want - Have);
		else Teams->RemoveTeamTagStack(LyraTeam, ElimTag, Have - Want);
	};
	Sync(wire::kTeamA, ScoreA);
	Sync(wire::kTeamB, ScoreB);
}

// 타이머·목표 점수는 블루프린트(B_TeamDeathMatchScoring) 변수라 이름으로 찾아 넣는다
void UMOBridgeSubsystem::ForceCountDown(int32 Seconds, int32 TargetScore)
{
	AGameStateBase* GS = GetWorld()->GetGameState();
	UActorComponent* Scoring = nullptr;
	if (GS)
	{
		for (UActorComponent* C : GS->GetComponents())
		{
			if (C && C->GetClass()->GetName().StartsWith(TEXT("B_TeamDeathMatchScoring")))
			{
				Scoring = C;
				break;
			}
		}
	}
	const auto Missing = [this](const TCHAR* What)
	{
		if (bLoggedScoringMissing) return;
		bLoggedScoringMissing = true;
		UE_LOG(LogMOB, Warning, TEXT("MOB_SCORING_MISSING %s"), What);
	};
	if (!Scoring)
	{
		Missing(TEXT("component"));
		return;
	}
	UClass* Cls = Scoring->GetClass();

	// Lyra 자기 타이머를 끈다 — 안 끄면 0 에서 저 혼자 경기를 끝낸다(핸들은 블루프린트 지역 값이라 이름으로 못 찾는다)
	GetWorld()->GetTimerManager().ClearAllTimersForObject(Scoring);

	const auto SetAndNotify = [&](const TCHAR* Prop, int32 Value, const TCHAR* Rep)
	{
		FNumericProperty* N = FindFProperty<FNumericProperty>(Cls, Prop);
		if (!N)
		{
			Missing(Prop);
			return;
		}
		void* Ptr = N->ContainerPtrToValuePtr<void>(Scoring);
		if (N->IsFloatingPoint()) N->SetFloatingPointPropertyValue(Ptr, (double)Value);
		else N->SetIntPropertyValue(Ptr, (int64)Value);
		if (UFunction* F = Cls->FindFunctionByName(Rep)) Scoring->ProcessEvent(F, nullptr);	// 화면이 이 알림으로 갱신된다
		else Missing(Rep);
	};
	SetAndNotify(TEXT("TargetScore"), TargetScore, TEXT("OnRep_TargetScore"));
	SetAndNotify(TEXT("CountDownTime"), Seconds, TEXT("OnRep_CountDownTime"));
	++CountDownSets;
	LastCountDownShown = Seconds;
}

void UMOBridgeSubsystem::ApplyMatch(const wire::Match& M)
{
	++MatchesRecv;
	if (M.phase != MatchPhase)
	{
		++PhaseChanges;
		UE_LOG(LogMOB, Log, TEXT("MOB_MATCH tick=%u phase %u->%u left=%.1f target=%u score=%u:%u lyraBefore=%d:%d"), M.tick, MatchPhase, M.phase, M.secondsLeft, M.targetScore, M.teamScoreA, M.teamScoreB, LyraScoreA, LyraScoreB);
	}
	if (M.phase != MatchPhase && M.phase == (uint8)wire::EMatchPhase::Playing) ServerAssists.Reset();	// 서버도 경기 시작에 도움 수를 0 으로(Room.cpp) — 대기 중엔 피해가 없다
	MatchPhase = M.phase;
	ScoreA = M.teamScoreA;
	ScoreB = M.teamScoreB;
	SyncTeamScores();
	if (const ULyraTeamSubsystem* Teams = GetWorld()->GetSubsystem<ULyraTeamSubsystem>())
	{
		static const FGameplayTag ElimTag = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.Score.Eliminations"));
		LyraScoreA = Teams->GetTeamTagStackCount(LyraTeamOf(wire::kTeamA), ElimTag);
		LyraScoreB = Teams->GetTeamTagStackCount(LyraTeamOf(wire::kTeamB), ElimTag);
	}

	if (!bPostGamePrepared)
	{
		// 끝 단계가 끝나면 Lyra 는 맵을 다시 연다 — 경기 끝은 서버 몫이고 결과 화면을 봐야 하니 그 시간을 길게 둔다
		bPostGamePrepared = true;
		UClass* PostPhase = LoadClass<ULyraGamePhaseAbility>(nullptr, TEXT("/ShooterCore/Experiences/Phases/Phase_PostGame.Phase_PostGame_C"));
		FNumericProperty* N = PostPhase ? FindFProperty<FNumericProperty>(PostPhase, TEXT("NewGameTime")) : nullptr;
		if (N)
		{
			void* Ptr = N->ContainerPtrToValuePtr<void>(PostPhase->GetDefaultObject());
			if (N->IsFloatingPoint()) N->SetFloatingPointPropertyValue(Ptr, 86400.0);
			else N->SetIntPropertyValue(Ptr, (int64)86400);
		}
		else
		{
			UE_LOG(LogMOB, Warning, TEXT("MOB_SCORING_MISSING NewGameTime"));
		}
	}

	const int32 Seconds = FMath::CeilToInt(M.secondsLeft);
	ForceCountDown(Seconds, M.targetScore);

	if ((wire::EMatchPhase)M.phase == wire::EMatchPhase::Warmup)
	{
		// 시작 전 숫자 — Lyra 대기 단계가 보내던 알림을 서버 남은 초로 보낸다
		static const FGameplayTag CountdownTag = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.GamePhase.MatchBeginCountdown"), false);
		if (CountdownTag.IsValid())
		{
			FLyraVerbMessage Msg;
			Msg.Verb = CountdownTag;
			Msg.Magnitude = (double)Seconds;
			UGameplayMessageSubsystem::Get(this).BroadcastMessage(CountdownTag, Msg);
		}
	}
	else if ((wire::EMatchPhase)M.phase >= wire::EMatchPhase::PostGame)
	{
		ULyraGamePhaseSubsystem* Phases = GetWorld()->GetSubsystem<ULyraGamePhaseSubsystem>();
		static const FGameplayTag PostTag = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.GamePhase.PostGame"));
		if (Phases && !Phases->IsPhaseActive(PostTag))
		{
			if (UClass* PostPhase = LoadClass<ULyraGamePhaseAbility>(nullptr, TEXT("/ShooterCore/Experiences/Phases/Phase_PostGame.Phase_PostGame_C")))
			{
				Phases->StartPhase(PostPhase);
			}
		}
	}
}

void UMOBridgeSubsystem::LoadWeaponClasses()
{
	if (WeaponItemDefs.Num() != 0) return;
	static const TCHAR* const Names[(int32)mo::EWeapon::Count] = { TEXT("Pistol"), TEXT("Rifle"), TEXT("Shotgun") };
	for (const TCHAR* Name : Names)
	{
		const FString Item = FString::Printf(TEXT("/ShooterCore/Weapons/%s/ID_%s.ID_%s_C"), Name, Name, Name);
		const FString Equip = FString::Printf(TEXT("/ShooterCore/Weapons/%s/WID_%s.WID_%s_C"), Name, Name, Name);
		WeaponItemDefs.Add(LoadClass<ULyraInventoryItemDefinition>(nullptr, *Item));
		WeaponEquipDefs.Add(LoadClass<ULyraEquipmentDefinition>(nullptr, *Equip));
		UE_LOG(LogMOB, Log, TEXT("MOB_WEAPON_CLASS %s item=%d equip=%d"), Name, WeaponItemDefs.Last() ? 1 : 0, WeaponEquipDefs.Last() ? 1 : 0);
	}
}

// 내 무기 — 첫 판 범위 셋을 빠른 칸에 넣어 두고, 고른 칸·탄은 서버 값에 맞춘다
void UMOBridgeSubsystem::SyncSelfWeapon(const wire::PlayerState& P)
{
	if (P.flags & wire::STATE_DEAD)
	{
		bWasReloading = false;
		return;
	}
	const bool bReloading = (P.flags & wire::STATE_RELOADING) != 0;
	if (bReloading && !bWasReloading && P.weapon < 3)
	{
		ReloadWeapon = P.weapon;
		ReloadMagBefore = P.magazine;
		++ReloadStarts[P.weapon];
	}
	if (!bReloading && bWasReloading && P.weapon == ReloadWeapon && P.magazine > ReloadMagBefore) ++ReloadDones[ReloadWeapon];
	bJustReloaded = !bReloading && bWasReloading;
	bWasReloading = bReloading;

	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	ULyraQuickBarComponent* Bar = PC ? PC->FindComponentByClass<ULyraQuickBarComponent>() : nullptr;
	ULyraInventoryManagerComponent* Inv = PC ? PC->FindComponentByClass<ULyraInventoryManagerComponent>() : nullptr;
	if (!Bar || !Inv || !PC->GetPawn() || P.weapon >= (uint8)mo::EWeapon::Count) return;
	LoadWeaponClasses();

	int32 WantSlot = INDEX_NONE;
	for (int32 W = 0; W < WeaponItemDefs.Num(); ++W)
	{
		UClass* Def = WeaponItemDefs[W];
		if (!Def) continue;
		int32 Found = INDEX_NONE;
		const TArray<ULyraInventoryItemInstance*> Slots = Bar->GetSlots();
		for (int32 S = 0; S < Slots.Num(); ++S)
		{
			if (Slots[S] && Slots[S]->GetItemDef() == Def) Found = S;
		}
		if (Found == INDEX_NONE)
		{
			Found = Bar->GetNextFreeItemSlot();
			if (Found == INDEX_NONE) continue;
			if (ULyraInventoryItemInstance* Item = Inv->AddItemDefinition(Def, 1)) Bar->AddItemToSlot(Found, Item);
			UE_LOG(LogMOB, Log, TEXT("MOB_QUICKBAR_ADD weapon=%d slot=%d"), W, Found);
		}
		if (W == (int32)P.weapon) WantSlot = Found;
	}
	if (WantSlot == INDEX_NONE) return;
	if (Bar->GetActiveSlotIndex() != WantSlot)
	{
		++WeaponSwaps;
		Bar->SetActiveSlotIndex(WantSlot);
	}

	ULyraInventoryItemInstance* Active = Bar->GetActiveSlotItem();
	if (!Active) return;
	static const FGameplayTag MagTag = FGameplayTag::RequestGameplayTag(TEXT("Lyra.ShooterGame.Weapon.MagazineAmmo"));
	static const FGameplayTag SpareTag = FGameplayTag::RequestGameplayTag(TEXT("Lyra.ShooterGame.Weapon.SpareAmmo"));
	const auto SetStack = [&](const FGameplayTag& Stack, int32 Want)
	{
		const int32 Have = Active->GetStatTagStackCount(Stack);
		if (Have == Want) return;
		++AmmoFixes;
		if (Have < Want) Active->AddStatTagStack(Stack, Want - Have);
		else Active->RemoveStatTagStack(Stack, Have - Want);
	};
	// 지난 상태에서 Lyra 탄을 서버 값으로 맞춰 뒀다 — 지금까지 Lyra 가 쏜 수 ↔ 서버가 쏜 수. 재장전 중·끝난 직후·무기 바꾼 직후는 뺀다
	const int32 MagHave = Active->GetStatTagStackCount(MagTag);
	if (!bReloading && !bJustReloaded && LastSyncWeapon == P.weapon && LastSyncMag >= 0)
	{
		const int32 LyraShot = FMath::Max(0, LastSyncMag - MagHave);
		const int32 ServerShot = FMath::Max(0, LastSyncMag - (int32)P.magazine);
		// 무적은 시작 전·끝 단계에도 켜진다 — 부활 무적만 따로 보려고 경기 중과 나머지를 가른다
		const bool bPlaying = MatchPhase == (uint8)wire::EMatchPhase::Playing;
		const bool bImmune = (P.flags & wire::STATE_IMMUNE) != 0;
		if (!bPlaying)
		{
			BulletsLyraOther += LyraShot;
			BulletsServerOther += ServerShot;
		}
		else if (bImmune)
		{
			BulletsLyraImmune += LyraShot;
			BulletsServerImmune += ServerShot;
		}
		else
		{
			BulletsLyra += LyraShot;
			BulletsServer += ServerShot;
		}
		MagDiffMax = FMath::Max(MagDiffMax, FMath::Abs(MagHave - (int32)P.magazine));
	}
	LastSyncWeapon = P.weapon;
	LastSyncMag = P.magazine;
	SetStack(MagTag, P.magazine);
	SetStack(SpareTag, P.spare);
}

void UMOBridgeSubsystem::SyncRemoteWeapon(FMOBRemote& R, ACharacter* Ch)
{
	if (R.ShownWeapon == R.Weapon || R.Weapon >= (uint8)mo::EWeapon::Count) return;
	ULyraEquipmentManagerComponent* EM = Ch->FindComponentByClass<ULyraEquipmentManagerComponent>();
	if (!EM) return;
	LoadWeaponClasses();
	UClass* Def = WeaponEquipDefs.IsValidIndex(R.Weapon) ? WeaponEquipDefs[R.Weapon].Get() : nullptr;
	if (!Def) return;
	if (ULyraEquipmentInstance* Old = R.Equipped.Get()) EM->UnequipItem(Old);
	R.Equipped = EM->EquipItem(Def);
	R.ShownWeapon = R.Weapon;
	++RemoteEquips;
}

void UMOBridgeSubsystem::HandleShot(const wire::Shot& M)
{
	++ShotsRecv;
	if (M.shooter == PlayerId && M.weapon < 3) ++MyShots[M.weapon];
	const int32 Count = FMath::Min<int32>(M.count, wire::kMaxBulletsInShot);
	if (Count <= 0) return;

	if (M.shooter == PlayerId)
	{
		// 맞았는지는 서버 답으로 표시한다 — 내 총알의 불꽃·궤적은 Lyra 자기 능력이 그린다
		APlayerController* PC = GetWorld()->GetFirstPlayerController();
		ULyraWeaponStateComponent* WS = PC ? PC->FindComponentByClass<ULyraWeaponStateComponent>() : nullptr;
		if (!WS) return;
		TArray<FHitResult> Hits;
		for (int32 i = 0; i < Count; ++i)
		{
			const wire::ShotBullet& B = M.bullets[i];
			APawn* Target = B.hit == (uint8)wire::EShotHit::Body && B.damage > 0.f ? FindPlayerPawn(B.target) : nullptr;
			if (!Target) continue;
			FHitResult& Hit = Hits.AddDefaulted_GetRef();
			Hit.Location = Hit.ImpactPoint = FVector(B.x, B.y, B.z);
			Hit.HitObjectHandle = FActorInstanceHandle(Target);
		}
		if (Hits.Num() == 0) return;
		FGameplayAbilityTargetDataHandle Handle;
		Handle.UniqueId = ++HitMarkerId;
		WS->AddUnconfirmedServerSideHitMarkers(Handle, Hits);
		WS->ClientConfirmTargetData(Handle.UniqueId, true, TArray<uint8>());
		++HitMarkers;
		RequestEventShot(TEXT("hit"), 0.05, 3);
		return;
	}

	// 남의 총 — Lyra 불꽃·궤적·탄착 효과를 서버가 정한 점으로 부른다
	APawn* Shooter = FindPlayerPawn(M.shooter);
	UAbilitySystemComponent* ASC = FindASC(Shooter);
	if (!ASC) return;
	static const TCHAR* const Names[(int32)mo::EWeapon::Count] = { TEXT("Pistol"), TEXT("Rifle"), TEXT("Shotgun") };
	if (M.weapon >= (uint8)mo::EWeapon::Count) return;
	const FGameplayTag FireTag = FGameplayTag::RequestGameplayTag(*FString::Printf(TEXT("GameplayCue.Weapon.%s.Fire"), Names[M.weapon]), false);
	if (!FireTag.IsValid()) return;
	const FVector Start(M.startX, M.startY, M.startZ);
	for (int32 i = 0; i < Count; ++i)
	{
		const FVector End(M.bullets[i].x, M.bullets[i].y, M.bullets[i].z);
		FGameplayCueParameters Cue;
		Cue.Location = End;
		Cue.Normal = (Start - End).GetSafeNormal();
		Cue.Instigator = Shooter;
		Cue.EffectCauser = Shooter;
		Cue.EffectContext = ASC->MakeEffectContext();
		ASC->ExecuteGameplayCue(FireTag, Cue);
		++ShotCues;
	}
}

UAbilitySystemComponent* UMOBridgeSubsystem::FindASC(AActor* Actor) const
{
	return Actor ? UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Actor) : nullptr;
}
