#include "MOBridgeSubsystem.h"

#include "GameProtocol.h"
#include "Grenade.h"
#include "MapCollision.h"
#include "MatchRules.h"
#include "Movement.h"
#include "Prediction.h"
#include "Pickups.h"
#include "Weapon.h"
#include "AnimClips.h"
#include "AnimPose.h"
#include "AnimState.h"

#include <algorithm>
#include <vector>

#include "AIController.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Components/CapsuleComponent.h"
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
#include "EnhancedInputComponent.h"
#include "InputAction.h"
#include "InputKeyEventArgs.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
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
#include "AbilitySystem/Abilities/LyraAbilitySimpleFailureMessage.h"
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
#include "AbilitySystem/LyraAbilityTagRelationshipMapping.h"
#include "Interaction/LyraInteractionDurationMessage.h"
#include "LyraGameplayTags.h"
#include "EngineUtils.h"
#include "Weapons/LyraWeaponSpawner.h"
#include "Inventory/LyraInventoryItemDefinition.h"
#include "Equipment/LyraPickupDefinition.h"
#include "UObject/UnrealType.h"
#include "GameplayCueFunctionLibrary.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "GameplayCueManager.h"
#include "GameplayCueSet.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"
#include "TimerManager.h"
#include "Components/StaticMeshComponent.h"
#include "Components/AudioComponent.h"
#include "Components/LightComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Particles/ParticleSystemComponent.h"
#include "Messages/LyraVerbMessage.h"
#include "Player/LyraPlayerState.h"
#include "Teams/LyraTeamSubsystem.h"
#include "Weapons/LyraWeaponStateComponent.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"

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

	// 사람 확인 각본(-MOBHumanCheck, human-check-auto-plan.md) — 칸마다 할 일 · 길이(산 채 초) · 무기(EWeapon). bBlock = 죽으면 다시 시작할 구역의 첫 칸,
	// bSample = 카메라 · 발 높이를 재는 칸. 길이는 제 판단: 쥐는 시간 · 조준 섞기(0.22초) · 샷건 사격 간격(fireDelay 0.5초)보다 넉넉하게
	enum class EHcAct : uint8 { Face, AdsHold, AdsFire, Reload, HipFire, Still, CrouchPress, CrouchWalk, CrouchJump, StandUp, Approach, MeleeHold };
	struct FHcStep { const TCHAR* Name; EHcAct Act; double Seconds; uint8 Weapon; bool bBlock; bool bSample; };
	const FHcStep kHumanSteps[] = {
		{ TEXT("face"),          EHcAct::Face,        2.5, 2, true,  false },	// 샷건을 쥐고 벽 방향을 고른다
		{ TEXT("ads_hold"),      EHcAct::AdsHold,     0.6, 2, false, false },
		{ TEXT("ads_fire"),      EHcAct::AdsFire,    11.0, 2, false, false },	// 조준한 채 8발(탄창 8) — 1.3초마다 누름
		{ TEXT("reload"),        EHcAct::Reload,      3.5, 2, false, false },
		{ TEXT("hip_fire"),      EHcAct::HipFire,    11.0, 2, false, false },	// 허리로 8발 — 조준 칸과 같은 간격(같은 열 흐름)
		{ TEXT("stand"),         EHcAct::Still,       2.5, 1, true,  true  },	// 소총으로 바꿔 선 채
		{ TEXT("crouch"),        EHcAct::CrouchPress, 2.0, 1, false, true  },
		{ TEXT("crouch_walk"),   EHcAct::CrouchWalk,  1.5, 1, false, false },
		{ TEXT("crouch_jump"),   EHcAct::CrouchJump,  1.5, 1, false, false },
		{ TEXT("stand_up"),      EHcAct::StandUp,     1.0, 1, false, false },
		// 다가가기 — 가장 가까운 살아 있는 적에게 맵 길찾기로 걸어가 몸이 닿으면 일찍 끝(최대 40초). 미는 봇은 곧장만 걸어 구조물에 막힌다 —
		// 파일럿 ha3 은 각본 50초 동안 한 번도 안 닿아 각본 근접 19번이 전부 허공이었다. 끝 조건을 1.5m 로 둔 판(ha4 · hb1 · hb2)은 근접이 닿기 전에
		// 적을 쓰러뜨려 몸이 한 번도 안 닿은 판(hb1 닿음 0)이 나왔고, 첫 다가가기는 네 판 모두 20초에 35m 남아 못 닿았다 — 다음 다가가기는 14초에 닿음. 40초는 제 판단
		{ TEXT("approach_pistol"),  EHcAct::Approach, 40.0, 0, true,  false },
		{ TEXT("melee_pistol"),     EHcAct::MeleeHold, 4.5, 0, false, false },	// 1초 쥐고 3.5초 누른 채
		{ TEXT("approach_rifle"),   EHcAct::Approach, 40.0, 1, true,  false },
		{ TEXT("melee_rifle"),      EHcAct::MeleeHold, 4.5, 1, false, false },
		{ TEXT("approach_shotgun"), EHcAct::Approach, 40.0, 2, true,  false },
		{ TEXT("melee_shotgun"),    EHcAct::MeleeHold, 4.5, 2, false, false },
	};
	constexpr int32 kHumanStepCount = UE_ARRAY_COUNT(kHumanSteps);
	constexpr double kHcFireEvery = 1.3;
	constexpr double kHcMeleeFrom = 1.0;
	constexpr double kHcApproachStraight = 300.0;	// 이 거리(cm, 수평) 안이면 길찾기 없이 곧장 밀고 들어간다 — 몸이 닿을 때까지
	constexpr double kHcSettle = 0.5;	// 앉음이 바뀐 뒤 재기 시작 — 카메라 섞기 0.2초(LyraCameraMode_ThirdPerson 배율 5)보다 넉넉히

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

	// 누름 표식 갈고리(L-5-c) — Lyra 는 다리를 모른다. 사격 갈고리(GBulletBridge)는 -MOBAbilityOwnTimer 면 안 거므로 따로 둔다
	UMOBridgeSubsystem* GPressBridge = nullptr;

	void InputTagPressedHook(const ULyraAbilitySystemComponent& Self, const FGameplayTag& InputTag)
	{
		if (GPressBridge) GPressBridge->OnLyraInputTagPressed(Self, InputTag);
	}

	// wire::EPressKey 차례(근접 · 대시 · 수류탄 · 재장전)의 Lyra 입력 표식 · 묶음 이름 · 글자. 표식은 다리 다른 곳(자동 각본 · 상대 대조)과 같은 이름이다
	const TCHAR* const kPressKeyTags[wire::kPressKeyCount] = { TEXT("InputTag.Ability.Melee"), TEXT("InputTag.Ability.Dash"), TEXT("InputTag.Weapon.Grenade"), TEXT("InputTag.Weapon.Reload") };
	const TCHAR* const kPressKeyActions[wire::kPressKeyCount] = { TEXT("IA_Melee"), TEXT("IA_Ability_Dash"), TEXT("IA_Grenade"), TEXT("IA_Weapon_Reload") };
	const TCHAR kPressKeyLetters[wire::kPressKeyCount] = { 'M', 'D', 'G', 'R' };

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
	// Lyra 대시 능력(GA_Hero_Dash) 칸 — 근접과 같은 까닭으로 손잡이로 기억한다(TickAbilityDash)
	FGameplayAbilitySpecHandle GDashSpecHandle;
	// 예측이 켠 대시를 Lyra 가 못 켤 때 다시 해 보는 프레임 상한. Lyra 쿨다운 1.5초가 예측 91입력(1.517초)보다 짧지만, 다리는 입력을 만든 프레임에 켜므로
	// 앞 대시를 늦은 프레임(끊김)에 켰으면 두 켬의 간격이 1.5초 아래로 줄 수 있다 — 근접과 같은 여섯(제 판단). 넘으면 그 대시는 Lyra 몽타주 없이 지나간다(DashDropped)
	constexpr uint64 kMaxDashWaitFrames = 6;
	// Lyra 수류탄 능력(GA_Grenade) 칸 — 표식을 뗀 뒤 손잡이로 기억한다(TickAbilityGrenade). 다리는 이 능력을 켜지 않고 켜지는지만 본다
	FGameplayAbilitySpecHandle GGrenadeSpecHandle;
	// 쿨다운 메시지의 남은 시간 — Lyra 는 켤 때 쿨다운 효과를 걸고 그 남은 시간(GetCooldownTimeRemaining)을 싣는다 = 효과 길이
	// (GE_Grenade_Cooldown DurationMagnitude 5.0, 원본 기준 C:\tmp\mob\gren0\t3d\GE_Grenade_Cooldown_cdo.t3d · ga_flow.txt:22-29)
	constexpr float kLyraGrenadeCooldownSeconds = 5.0f;
	// 몽타주 칸을 능력 클래스에서 못 읽을 때 쓰는 자산(GA_Grenade CDO 의 GrenadeThrowMontage 값, 원본 기준 GA_Grenade_cdo.t3d)
	const TCHAR* const kGrenadeThrowMontagePath = TEXT("/Game/Characters/Heroes/Mannequin/Animations/Actions/AM_MM_Rifle_GrenadeToss.AM_MM_Rifle_GrenadeToss");
	// 겉모습 수류탄(G-5-b) — 능력이 만드는 클래스 · 부딪힘 소리 · 소리 감쇠(원본 기준 C:\tmp\mob\gren0\ga_flow.txt:2-10 · b_flow.txt:131-139)
	const TCHAR* const kGrenadeActorClassPath = TEXT("/ShooterCore/Weapon/Grenade/B_Grenade.B_Grenade_C");
	const TCHAR* const kGrenadeImpactSoundPath = TEXT("/Game/Audio/MetaSounds/sfx_Weapon_GrenadeImpact_nl_meta.sfx_Weapon_GrenadeImpact_nl_meta");
	const TCHAR* const kGrenadeImpactAttenuationPath = TEXT("/Game/Audio/AttenuationPresets/ATT_Projectile.ATT_Projectile");
	const TCHAR* const kGrenadeDetonateCueTag = TEXT("GameplayCue.Weapon.Grenade.Detonate");	// B_Grenade 변수 Gameplay Tag(원본 기준 B_Grenade_cdo.t3d)
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

	// 이동 방식을 바꾸다 엔진이 옮긴 자리를 되돌린 수 · 최대 거리(내 몸 · 남의 몸 합 — MOB_SUMMARY3 modeSwitchRestores). 밀림 0 인 판이 착지가 없었던 판인지 가른다
	int64 GModeSwitchRestores = 0;
	double GModeSwitchRestoreMax = 0.0;

	// 자리는 부르는 쪽이 이미 놓았다. 엔진은 걷기로 들어갈 때 바닥을 찾고(FindFloor) 캡슐을 바닥 틈 1.9~2.4cm 로 옮긴다(AdjustFloorHeight —
	// CharacterMovementComponent.cpp OnMovementModeChanged). 화면 자리는 두 입력 · 두 상태 사이를 메운 값이라 착지 순간 그 틈 밖에 있어 한 프레임 튀었다(L-9-b —
	// 내 밀림 1cm 넘는 5번이 다 이 옮김, 최대 23.1cm). 바닥 찾기 · 바탕 정하기는 놓은 자리에서 그대로 하고 옮김만 되돌린다
	void ApplyMoveToCharacter(ACharacter* Ch, const FVector& Velocity, bool bFalling)
	{
		UCharacterMovementComponent* M = Ch->GetCharacterMovement();
		if (!M) return;
		if (bFalling != M->IsFalling())
		{
			const FVector Placed = Ch->GetActorLocation();
			M->SetMovementMode(bFalling ? MOVE_Falling : MOVE_Walking);
			const double Moved = FVector::Dist(Ch->GetActorLocation(), Placed);
			if (Moved > 0.0)
			{
				Ch->SetActorLocation(Placed, false, nullptr, ETeleportType::TeleportPhysics);
				++GModeSwitchRestores;
				GModeSwitchRestoreMax = FMath::Max(GModeSwitchRestoreMax, Moved);
			}
		}
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
	// 산 수류탄 몸(L-8-b-2 · D138 · D139) — 마지막 수류탄 상태 소식의 칸으로 만든 걸음 몸(봇 SetGrenadeBodiesForPrediction 과 같은 규칙 — 와이어 값 그대로 ·
	// 제 수류탄은 그 소식 틱 걸음 뒤 launchCleanedUp 전이면 뺌 · 터짐 소식이면 뺌 · 다음 틱 목록이 안 오면 닫고 비움). 사람끼리 막기 규칙과 따로 늘 건다(수류탄은 맵 몸, L-2-b)
	std::vector<mo::MapCapsule> GrenadeBodies;
	uint32_t GrenadeListTick = 0;
	bool bGrenadeListOpen = false;
	uint32_t ConfirmedSeq = 0;		// 마지막 내 상태의 lastSeq — 몸이 바뀌어 다시 돌릴 입력의 앞

	// 예측이 걸음을 돌리는 동안만 넘긴다 — Sim 을 쓰는 다른 곳(근접 화면 판정 등)에 남지 않게
	struct FPawnScope
	{
		FMOBCore& C;
		explicit FPawnScope(FMOBCore& InC) : C(InC)
		{
			if (C.Rules.pawnBlock) C.Sim.SetPawns(C.Pawns.data(), C.Pawns.size());
			C.Sim.SetStandables(C.GrenadeBodies.data(), C.GrenadeBodies.size());
		}
		~FPawnScope()
		{
			C.Sim.SetPawns(nullptr, 0);
			C.Sim.SetStandables(nullptr, 0);
		}
	};

	// 몸이 바뀌었다 — 서버가 아직 안 돌린 입력을 새 몸으로 다시 돌린다(봇 RerunForGrenadeBodies 와 같다 — 마지막으로 돌린 몸 = 서버가 그 입력에 쓴 몸)
	bool RerunForGrenadeBodies()
	{
		const FPawnScope Scope(*this);
		return Prediction.RerunUnconfirmed(ConfirmedSeq);
	}

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
		if (ULyraAbilitySystemComponent::MOBInputTagPressedHook == &InputTagPressedHook) ULyraAbilitySystemComponent::MOBInputTagPressedHook = nullptr;
		GPressBridge = nullptr;
		delete Core;
		Core = nullptr;
	}
	if (GrenadeCueRouteHandle.IsValid())
	{
		if (UGameplayCueManager* GCM = UAbilitySystemGlobals::Get().GetGameplayCueManager()) GCM->OnGameplayCueRouted().Remove(GrenadeCueRouteHandle);
		GrenadeCueRouteHandle.Reset();
	}
	if (FailMsgHandle.IsValid()) FailMsgHandle.Unregister();
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
	bHumanCheck = FParse::Param(Cmd, TEXT("MOBHumanCheck"));
	bWeaponScript = FParse::Param(Cmd, TEXT("MOBWeaponScript"));
	if (bWeaponScript) bAutoInput = true;		// 각본이 자동 입력 깃발(재장전 · 칸)을 쓴다 — 키보드는 안 본다
	FParse::Value(Cmd, TEXT("MOBBreakBag="), BreakBag);
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
	bStandLyra = bStandOnly && FParse::Param(Cmd, TEXT("MOBStandLyra"));
	bServerAnim = FParse::Param(Cmd, TEXT("MOBServerAnim"));
	bSimpleAnim = bServerAnim || FParse::Param(Cmd, TEXT("MOBSimpleAnim"));
	bAnimLog = FParse::Param(Cmd, TEXT("MOBAnimLog"));
	bViewLog = FParse::Param(Cmd, TEXT("MOBViewLog"));
	bRayCheck = FParse::Param(Cmd, TEXT("MOBRayCheck"));
	bAbilityOwnTimer = FParse::Param(Cmd, TEXT("MOBAbilityOwnTimer"));
	bBreakCrouchBlock = FParse::Param(Cmd, TEXT("MOBBreakCrouchBlock"));
	bBreakMeleeBlock = FParse::Param(Cmd, TEXT("MOBBreakMeleeBlock"));
	bBreakDevices = FParse::Param(Cmd, TEXT("MOBBreakDevices"));
	bAutoMeleeOn = FParse::Param(Cmd, TEXT("MOBAutoMelee"));
	bBreakDashBlock = FParse::Param(Cmd, TEXT("MOBBreakDashBlock"));
	bBreakDashDir = FParse::Param(Cmd, TEXT("MOBBreakDashDir"));
	bAutoDashOn = FParse::Param(Cmd, TEXT("MOBAutoDash"));
	bBreakGrenadeBlock = FParse::Param(Cmd, TEXT("MOBBreakGrenadeBlock"));
	bAutoGrenadeOn = FParse::Param(Cmd, TEXT("MOBAutoGrenade"));
	FParse::Value(Cmd, TEXT("MOBBreakPressOrder="), BreakPressOrder);
	bPressKeyTest = FParse::Param(Cmd, TEXT("MOBPressKeyTest"));
	bBreakGrenadeVisual = FParse::Param(Cmd, TEXT("MOBBreakGrenadeVisual"));
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
	// 누름 차례(L-5-c) — 이 틱 앞(사람 키 입력 처리 · 지난 틱 자동 각본)에 Lyra 가 부른 표식을 이번 입력 몫으로 넘긴다. Lyra 도 그것을 이 프레임 능력 입력 처리에서 본다
	FramePressKeys.Append(PendingPressKeys);
	PendingPressKeys.Reset();
	if (FramePressKeys.Num() > 64)
	{
		PressTagsDropped += FramePressKeys.Num() - 64;
		FramePressKeys.RemoveAt(0, FramePressKeys.Num() - 64);
	}
	if (bAutoInput && !(bHumanCheck && TickHumanScript(T)))
	{
		if (bWeaponScript) TickWeaponScript(T);
		else TickAutoInput(T);
	}
	TickInput(Now);
	if (bPressKeyTest) TickPressKeyTest(T);	// 입력을 만든 뒤 — 넣은 키 사건은 다음 프레임 앞 입력 처리가 받는다(사람 키가 들어오는 자리)
	TickAbilityFire();
	TickAbilityMelee();
	TickAbilityDash();
	TickAbilityGrenade();
	if (bHumanCheck) SampleHumanCheck(T);
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
	// 장치 찾아 끄기는 몸을 놓기 전에 — 같은 프레임에 놓인 몸이 아직 켜진 장치와 먼저 겹치지 않게(D91, 내 몸 서기 판 stdev_Lup_off 에서 겹침 58프레임)
	if (Now >= NextDeviceCheckAt)
	{
		NextDeviceCheckAt = Now + 1.0;
		FindDevices();
	}
	TickSelfVisual(Now, DeltaTime);
	TickRemotes(Now);
	TickRemoteDashCues();
	TickGrenadeVisuals();
	TickPickups(Now);
	TickDevices(Now);
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

	// 사람이면 결과 화면을 볼 수 있게 둔다. -MOBHumanCheck 는 결과 뒤 점수판을 열고 1.2초에 찍으므로 3초(제 판단)
	if (ResultAt > 0 && bAutoInput && Now - ResultAt > (bHumanCheck ? 3.0 : 1.5)) FinishAndQuit(TEXT("result"));
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
	GPressBridge = this;
	ULyraAbilitySystemComponent::MOBInputTagPressedHook = &InputTagPressedHook;
	if (BreakPressOrder != 0) UE_LOG(LogMOB, Warning, TEXT("MOB_PRESS_BREAK mode=%d"), BreakPressOrder);

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
			Weapon = 0;			// 부활 가방 = [권총, …] 의 권총(Loadout Reset) — 세 무기 시험 옵션이어도 든 것은 권총
			AutoWeapon = 0;
			LastSelfSpawnAt = FPlatformTime::Seconds();
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
					Core->Prediction.OnSpawn(M.x, M.y, M.z, M.lastSeq, M.yaw);	// 요 = 대시의 「앞 입력」 요(서버 SpawnPlayer 와 같다)
				}
				NextStandAt = 0;	// 촬영 전용 — 시작 자리로 돌아갔으면 바로 다시 옮긴다(맵이 달라 시작 자리는 허공이다)
				bStandPlaced = false;	// 받침 시험(-MOBStandHp) — 새 몸도 다친 채 선다(경기 시작 부활이 체력 100 으로 다시 세웠다, B-3 pb1)
				SelfCur = ToFVector(Core->Prediction.GetMove().location);
				if (bStandLyra) SelfCur = StandAt;	// 서기 판은 입력이 없어 이 자리가 그대로 매 프레임 내 몸 자리가 된다(TickSelfVisual)
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
		// 산 수류탄 몸(L-8-b-2) — 지난 틱 목록이 안 왔으면 그 틱 끝 산 수류탄이 없었다 → 이 틱 입력은 몸 없이 돌았다. 닫고 비운 뒤 다시 돌린다(봇 OnState 와 같다).
		// 터짐 소식이 마지막 몸을 이미 뺐어도 돈다(그 전 다시 돌리기는 터진 몸을 넣었다)
		if (Core && Core->bGrenadeListOpen && Core->GrenadeListTick + 1 < M.tick)
		{
			Core->bGrenadeListOpen = false;
			if (Core->GrenadeBodies.empty()) ++GrenadeWalkEnded;
			else ++GrenadeWalkStale;
			Core->GrenadeBodies.clear();
			if (Core->RerunForGrenadeBodies()) ++GrenadeWalkReruns;
			else ++GrenadeWalkRerunsNone;
		}
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
			if (Core) Core->ConfirmedSeq = P.lastSeq;	// 몸 다시 돌리기(L-8-b-2)의 시작 앞 번호
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
	case wire::Msg::S2C_DASH:
	{
		wire::Dash M = wire::Dash(); FMemory::Memcpy(&M, Data, FMath::Min<int32>(Size, sizeof(M)));
		HandleDash(M);
		break;
	}
	case wire::Msg::S2C_GRENADE:
	{
		wire::Grenade M = wire::Grenade(); FMemory::Memcpy(&M, Data, FMath::Min<int32>(Size, sizeof(M)));
		HandleGrenade(M);
		break;
	}
	case wire::Msg::S2C_GRENADE_STATE:
		++GrenadeStatePackets;	// 겉모습 수류탄의 자리 · 부딪힘(G-5-b)
		HandleGrenadeState(Data, Size);
		break;
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
		// 가방 · 탄(W-4) — 봇 CompareSelf 와 같은 식. 가방은 줍기 · 부활만 바꾸고 둘 다 경기 끝 단계와 상관없어 늘 본다(무기 둘 이상 든 줄을 같이 센다).
		// 탄 · 든 무기도 늘 본다 — 서버가 경기 끝 단계에도 사격 입력을 돌린다(L-3-b. 그 전엔 지워서 끝 단계 앞까지만 봤다)
		++InvCompared;
		if (mo::CLoadout::UnpackSlot(P.quickSlots, 1) != mo::kNoWeapon) ++InvMulti;
		const bool bInvDiff = P.quickSlots != S->quickSlots;
		if (bInvDiff) ++InvDiff;
		++AmmoCompared;
		const bool bAmmoDiff = P.weapon != S->weapon || P.magazine != S->magazine || P.spare != S->spare;
		if (bAmmoDiff) ++AmmoDiff;
		if ((bInvDiff || bAmmoDiff) && InvDiffLinesLeft > 0)
		{
			--InvDiffLinesLeft;
			UE_LOG(LogMOB, Warning, TEXT("MOB_INV_DIFF seq=%u server=%u,%u,%d,%d predicted=%u,%u,%d,%d"), P.lastSeq, P.quickSlots, P.weapon, (int32)P.magazine, (int32)P.spare,
				S->quickSlots, S->weapon, (int32)S->magazine, (int32)S->spare);
		}
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
	// 대시 중 고침(D-5) — 서버 상태의 입력 번호가 예측이 켠 최근 대시부터 몇 입력 뒤인가(-1 = 앞에 켠 대시 없음)
	int64 DashAgo = -1;
	for (const uint32 DashSeq : DashStartSeqs)
	{
		if (DashSeq == 0 || DashSeq > P.lastSeq) continue;
		const int64 Ago = (int64)P.lastSeq - (int64)DashSeq;
		if (DashAgo < 0 || Ago < DashAgo) DashAgo = Ago;
	}
	if (DashAgo >= 0 && DashAgo < mo::kDashActiveInputs)
	{
		++DashCorrections;
		DashCorrectionMax = FMath::Max(DashCorrectionMax, Distance);
	}
	// 처음 80줄 + 3cm 이상은 늘 — 상한만 두면 판 뒤쪽 큰 보정이 안 남는다(판 hc1 최대 15.85cm 가 80줄 밖이었다)
	const uint64 HitsNow = Core->Sim.GetCounters().pawnHits;
	if (CorrLinesLeft > 0 || Distance >= 3.0)
	{
		if (CorrLinesLeft > 0) --CorrLinesLeft;
		const double NowSec = FPlatformTime::Seconds();
		double NearestRemote = -1.0;	// 고친 내 자리에서 가장 가까운 남 몸(화면에 그려진 자리, 수평 cm)
		for (const TPair<uint32, FMOBRemote>& Pair : Remotes)
		{
			if (!Pair.Value.Pawn.IsValid()) continue;
			const double D = FVector::Dist2D(After, Pair.Value.Pawn->GetActorLocation());
			if (NearestRemote < 0.0 || D < NearestRemote) NearestRemote = D;
		}
		// 크기 · 방향 · 서버 깃발 · 남이 나를 친 근접 / 내가 친 근접 뒤 몇 초(-1 = 없음) · 지난 보정 뒤 남 캡슐에 새로 닿은 수 · 가장 가까운 남
		UE_LOG(LogMOB, Log, TEXT("MOB_CORR t=%.2f seq=%u dist=%.2f d=(%.2f,%.2f,%.2f) flags=%u meleeOnMeAgo=%.2f myMeleeHitAgo=%.2f pawnHitsNew=%llu nearestRemote=%.1f dashAgo=%lld"),
			NowSec - ReadyAt, P.lastSeq, Distance, Delta.X, Delta.Y, Delta.Z, P.flags,
			LastMeleeOnMeAt > 0 ? NowSec - LastMeleeOnMeAt : -1.0, LastMyMeleeHitAt > 0 ? NowSec - LastMyMeleeHitAt : -1.0,
			(unsigned long long)(HitsNow - CorrLastPawnHits), NearestRemote, DashAgo);
	}
	CorrLastPawnHits = HitsNow;
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
		const FVector DriftMoved = Ch->GetActorLocation() - SelfLastSet;
		if (Drift > 1.0 && FMath::Abs(DriftMoved.Z - SelfSetMovedZ) < 0.01 && FVector2D(DriftMoved.X, DriftMoved.Y).Size() < 0.01) ++SelfDriftFromSetMove;
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
			// 대시(D-5) — Lyra 대시 능력이 켜져 있나 · 마지막으로 켜진 뒤 몇 프레임(-1 = 없음) · 캡슐 반높이(웅크림 바뀜이 자리를 옮기나)
			const UCapsuleComponent* Capsule = Ch->GetCapsuleComponent();
			// 까닭 가르기(L-9-b) — 자리 · 지난 놓기 프레임에 놓은 뒤 바뀐 높이 · 이동 방식 바꿈 앞뒤 · 앉음 바꿈 · Lyra 이동 컴포넌트 바닥(틈 · 걸을 수 있나 · 높이 · 물체) · 예측 방식 · 바닥 틈 · 높이
			const FFindFloorResult* LyraFloor = CMC ? &CMC->CurrentFloor : nullptr;
			const mo::MoveState& SimMove = Core->Prediction.GetMove();
			UE_LOG(LogMOB, Log, TEXT("MOB_SELF_DRIFT frame=%llu seq=%u drift=%.2f moved=(%.2f,%.2f,%.2f) lyraMelee=%d nearestRemote=%.1f mode=%d crouched=%d lyraDash=%d dashFramesAgo=%lld capsuleHalf=%.1f"
				" at=(%.2f,%.2f,%.2f) setMovedZ=%.2f setMode=%d>%d setCrouch=%d lyraFloorDist=%.2f lyraWalkable=%d lyraFloorZ=%.2f lyraFloor=%s simMode=%d simFloorDist=%.2f simZ=%.2f"),
				(unsigned long long)GFrameCounter, Seq, Drift, Moved.X, Moved.Y, Moved.Z, bLyraMeleeWasActive ? 1 : 0, NearestRemote,
				CMC ? (int32)CMC->MovementMode.GetValue() : -1, Ch->IsCrouched() ? 1 : 0, bLyraDashWasActive ? 1 : 0,
				DashLastStartFrame ? (long long)(GFrameCounter - DashLastStartFrame) : -1LL, Capsule ? Capsule->GetUnscaledCapsuleHalfHeight() : -1.0f,
				Ch->GetActorLocation().X, Ch->GetActorLocation().Y, Ch->GetActorLocation().Z, SelfSetMovedZ, SelfSetModeFrom, SelfSetModeTo, bSelfSetCrouchChanged ? 1 : 0,
				LyraFloor ? LyraFloor->FloorDist : -1.f, LyraFloor && LyraFloor->IsWalkableFloor() ? 1 : 0, LyraFloor ? LyraFloor->HitResult.ImpactPoint.Z : 0.0,
				LyraFloor ? *GetNameSafe(LyraFloor->HitResult.GetActor()) : TEXT("-"), (int32)SimMove.mode, SimMove.floor.floorDist, SimMove.location.z);
		}
	}

	const mo::MoveState& Move = Core->Prediction.GetMove();
	const bool bWaiting = Core->Prediction.IsWaitingSpawn();
	const double Alpha = FMath::Clamp(1.0 - (NextInputAt - Now) / kInputSeconds, 0.0, 1.0);
	VisualError *= FMath::Exp(-kVisualErrorDecay * DeltaTime);
	const FVector Shown = FMath::Lerp(SelfPrev, SelfCur, Alpha) + VisualError;

	// 앉음 = 예측의 지금 값(서버와 같은 코드). 자리는 직전 · 지금 입력 사이를 메우지만 앉음은 섞지 않는다 — 한 입력(1/60초) 안의 차이라 둔다(제 판단).
	// 캡슐 반높이가 Lyra 카메라 중심 높이에 들어가므로(ULyraCameraMode::GetPivotLocation) 자리 입히기 앞에 바꾼다
	bSelfSetCrouchChanged = ApplyCrouchToCharacter(Ch, Move.crouched);
	if (bSelfSetCrouchChanged) ++SelfCrouchChanges;
	Ch->SetActorLocation(Shown, false, nullptr, ETeleportType::TeleportPhysics);
	SelfLastSet = Ch->GetActorLocation();
	bSelfSet = true;
	const UCharacterMovementComponent* SetCMC = Ch->GetCharacterMovement();
	SelfSetModeFrom = SetCMC ? (int32)SetCMC->MovementMode.GetValue() : -1;
	ApplyMoveToCharacter(Ch, bWaiting ? FVector::ZeroVector : ToFVector(Move.velocity), !bWaiting && Move.mode == mo::EMoveMode::Falling);
	SelfSetModeTo = SetCMC ? (int32)SetCMC->MovementMode.GetValue() : -1;
	SelfSetMovedZ = Ch->GetActorLocation().Z - SelfLastSet.Z;
	if (FMath::Abs(SelfSetMovedZ) > 0.01)
	{
		++SelfSetMoves;
		SelfSetMoveMax = FMath::Max(SelfSetMoveMax, FMath::Abs(SelfSetMovedZ));
	}
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
		if (mo::HasHitReactMontage(Anim->States[Index]))
		{
			++AnimHitReactTicks;
			// 사람 확인 — 내 가까이(8m, 제 판단: 화면에서 움찔이 보일 거리)에서 맞은 반응이 든 남을 찍는다
			const APawn* MyPawn = GetWorld()->GetFirstPlayerController() ? GetWorld()->GetFirstPlayerController()->GetPawn() : nullptr;
			if (bHumanCheck && MyPawn && FVector::Dist(MyPawn->GetActorLocation(), Ch->GetActorLocation()) < 800.0) RequestEventShot(TEXT("react"), 0.05, 3);
		}
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
	AnimIdleSlotLayers += Result.IdleSlotLayers;
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
	// 거쳐 갈 칸 — 다른 무기 칸이 먼저, 없으면 빈 칸(권총 하나로 부활, D97 · W-5). 빈 칸을 고르면 쥔 무기만 내려놓고 아무것도 안 쥐었다가
	// (ULyraQuickBarComponent::EquipItemInSlot 은 빈 칸이면 아무것도 안 한다) 돌아올 때 다시 쥐어 층이 붙는다. 빈 칸을 안 보면 무기 하나일 때 다시 쥐기를 못 해
	// 첫 캐릭터가 무기 층 없이 남는다(W-5 human_auto 판 w5ha — emptyFrames 10026 · reequips 0)
	int32 Other = INDEX_NONE, Empty = INDEX_NONE;
	for (int32 S = 0; S < Slots.Num(); ++S)
	{
		if (S == Active) continue;
		if (Slots[S]) { Other = S; break; }
		if (Empty == INDEX_NONE) Empty = S;
	}
	const bool bViaEmpty = Other == INDEX_NONE;
	if (bViaEmpty) Other = Empty;
	if (Active == INDEX_NONE || Other == INDEX_NONE) return;
	SelfBodyNextTryAt = Now + REEQUIP_RETRY_SECONDS;
	++SelfReequipTries;
	++SelfReequips;
	Bar->SetActiveSlotIndex(Other);
	Bar->SetActiveSlotIndex(Active);	// 고른 칸은 그대로 — 서버 무기 번호와 같은 칸(SyncSelfWeapon)
	UE_LOG(LogMOB, Log, TEXT("MOB_SELFBODY_REEQUIP slot=%d via=%d layers=[%s] try=%d viaEmpty=%d"), Active, Other, *Layers.TrimEnd(), SelfReequipTries, bViaEmpty ? 1 : 0);
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
	const FVector RawMove = Move;	// 대시(D-5) — 다음 대시의 「앞 입력 이동」으로 Lyra 능력에 넣을 날 값(TickAbilityDash)
	InputRawMax = FMath::Max(InputRawMax, Move.Size());
	if (FMath::Abs(Move.X) > 0.01 && FMath::Abs(Move.Y) > 0.01) ++InputsDiagonal;
	if (Move.SizeSquared() > 1.0) Move.Normalize();
	InputSentMax = FMath::Max(InputSentMax, (double)FVector2f((float)Move.X, (float)Move.Y).Size());

	const FRotator Control = PC->GetControlRotation();
	const FVector Location = Ch->GetActorLocation();
	const FVector Aim = TargetingStart(PC, Location);
	// 수류탄 카메라 거리(입력 camBack, G-3) — 카메라에서 사격 시작점까지 시선 방향 거리 = (몸 − 카메라) · 시선(G-0 「camBack」 줄의 식 — 시작점은 시선 위 몸에 가장 가까운 점이라 같은 값).
	// 서버가 범위(kGrenadeCamBackMin ~ Max)로 자르고 던진 입력에만 쓴다(ClampGrenadeCamBack)
	float CamBack = 0.f;
	{
		FVector CamLoc;
		FRotator CamRot;
		PC->GetPlayerViewPoint(CamLoc, CamRot);
		CamBack = (float)((Location - CamLoc) | CamRot.Vector().GetSafeNormal());
	}

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
	// 대시 = 왼쪽 Shift(IMC_Default 의 IA_Ability_Dash, Pressed 트리거 — 원본 기준). 누른 입력 하나에만 단추를 싣는다(서버 · 예측도 누름 하나에 한 번)
	const bool bDashDown = bAutoInput ? bAutoDash : PC->IsInputKeyDown(EKeys::LeftShift);
	// 수류탄 = Q(IMC_ShooterGame 의 IA_Grenade, 원본 기준 C:\tmp\mob\gren0\t3d\IMC_ShooterGame.t3d). 누른 입력 하나에만 단추를 싣는다(서버 · 예측도 누름 하나에 한 번)
	const bool bGrenadeDown = bAutoInput ? bAutoGrenade : PC->IsInputKeyDown(EKeys::Q);
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

	// 키 1 · 2 · 3 = 빠른 칸 0 · 1 · 2(W-4, Lyra 빠른 칸 키와 같은 뜻) — 그 칸의 무기를 내 예측 가방(서버와 같은 함수로 바뀐다)에서 읽어 무기 번호로 보낸다.
	// 빈 칸이면 안 바꾼다(Lyra 도 빈 칸 고르기는 아무 일 없음). 와이어는 무기 번호 그대로 — 서버가 가방에 없는 번호를 거부한다
	uint8 Wanted = Weapon;
	int32 WantSlot = INDEX_NONE;
	if (bAutoInput)
	{
		if (AutoSlot >= 0) WantSlot = AutoSlot;
		else Wanted = AutoWeapon;
	}
	else if (PC->IsInputKeyDown(EKeys::One)) WantSlot = 0;
	else if (PC->IsInputKeyDown(EKeys::Two)) WantSlot = 1;
	else if (PC->IsInputKeyDown(EKeys::Three)) WantSlot = 2;
	if (WantSlot >= 0 && WantSlot < (int32)mo::kQuickSlotCount)
	{
		const uint8 SlotWeapon = Core->Prediction.GetLoadout().SlotWeapon((size_t)WantSlot);
		if (SlotWeapon != mo::kNoWeapon) Wanted = SlotWeapon;
	}
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
		In.camBack = CamBack;
		if (bJump) In.buttons |= wire::BUTTON_JUMP;
		if (bFire) In.buttons |= wire::BUTTON_FIRE;
		if (bReloadDown && !bReloadWasDown) { In.buttons |= wire::BUTTON_RELOAD; ++InputsReload; }
		bReloadWasDown = bReloadDown;
		if (bCrouchDown && !bCrouchWasDown) { In.buttons |= wire::BUTTON_CROUCH; ++InputsCrouch; }
		bCrouchWasDown = bCrouchDown;
		if (bAds) { In.buttons |= wire::BUTTON_ADS; ++InputsAds; }
		if (bMelee) { In.buttons |= wire::BUTTON_MELEE; ++InputsMelee; }
		// 경기 끝 단계에도 싣는다 — Lyra 는 능력 입력 막음(GE_BlockAbilityInput)을 맵을 다시 열기 한 틱 전에야 걸고(그래프 원본 · 기록 g0end1 · 2) 서버도 끝 단계에 켠다(L-3-b).
		// (그 전엔 서버가 끝 단계 내내 막아 싣지 않았다 — 판 dsh1 unconfirmed 6.) 봇 --dash 와 같은 규칙
		if (bDashDown && !bDashWasDown) { In.buttons |= wire::BUTTON_DASH; ++InputsDash; }
		bDashWasDown = bDashDown;
		// 수류탄(G-5-a) — 대시와 같다(끝 단계에도 싣는다 · L-3-b). GrenadePressPost = 그중 경기 끝 단계 누름
		if (bGrenadeDown && !bGrenadeWasDown)
		{
			In.buttons |= wire::BUTTON_GRENADE;
			++InputsGrenade;
			if (MatchPhase == (uint8)wire::EMatchPhase::PostGame) ++GrenadePressPost;
			GrenadeCamBackMin = FMath::Min(GrenadeCamBackMin, (double)CamBack);
			GrenadeCamBackMax = FMath::Max(GrenadeCamBackMax, (double)CamBack);
			// 서버가 자르는 범위(−90 ~ 417.3, L-3-c) 밖을 센다 — 옛 아래 끝 30 일 때 판 grn1 최소 17.25 · 서버 camBackClamped 1. 자세는 아래 AddInput 뒤 MOB_GRENADE_PRESS 줄로 본다
			// (판 grn2: 조준 없이 선 채 27.14 · 조준은 80 안팎 — 미결 25 → L-3-c 에서 아래 끝을 −90 으로)
			if (CamBack < mo::kGrenadeCamBackMin) ++GrenadeCamBackBelow;
			else if (CamBack > mo::kGrenadeCamBackMax) ++GrenadeCamBackAbove;
		}
		bGrenadeWasDown = bGrenadeDown;
		// 같은 입력 누름 차례(L-5-c) — 근접 · 대시 · 수류탄 · 재장전 중 둘 이상이면 Lyra 가 그 누름 표식을 부른 차례로 싣는다(근접 판정 WillMeleeStart 도 이 차례를 본다).
		// 부른 것은 이 입력 몫(FramePressKeys)으로 쓰고 비운다. 같은 프레임 두 번째 입력부터는 새 누름이 없다(키 가장자리는 첫 입력이 가져간다)
		FString PressText;
		In.pressOrder = MakeLyraPressOrder(In.buttons, PressText);
		FramePressKeys.Reset();
		const uint64 PressSpan = GFrameCounter - PressLastInputFrame;
		PressLastInputFrame = GFrameCounter;

		// 번호를 붙이고 여기서도 돌린다 — 서버 답을 기다리지 않고 바로 움직이려고
		// 근접(E-3) — 이 입력이 근접을 켜면 넣기 전에 화면 판정을 해, 맞음이면 밀기를 같이 넣는다(봇과 같은 길, D75). Lyra 근접 능력은 TickAbilityMelee 가 켠다
		const bool bMeleeStarts = Core->Prediction.WillMeleeStart(In);
		FMOBMeleeGuess MeleeGuess;
		mo::Vec3 MeleePush;
		if (bMeleeStarts) MeleeGuess = JudgeMelee(MeleePush);
		const uint64 DashStartedBefore = Core->Prediction.GetDashCounters().started;
		const uint64 GrenadeThrownBefore = Core->Prediction.GetGrenadeCounters().thrown;
		{
			const FMOBCore::FPawnScope PawnScope(*Core);	// 화면 판정(JudgeMelee) 뒤에 넘긴다 — 봇과 같은 차례
			Core->Prediction.AddInput(In, bMeleeStarts && MeleeGuess.Kind == wire::MELEE_HIT ? &MeleePush : nullptr);
		}
		Seq = In.seq;
		MeleeYaw = In.yaw;
		if (!PressText.IsEmpty() && PressLinesLeft > 0)
		{
			--PressLinesLeft;
			UE_LOG(LogMOB, Log, TEXT("MOB_PRESS_ORDER seq=%u frame=%llu span=%llu %s"), In.seq, (unsigned long long)GFrameCounter, (unsigned long long)PressSpan, *PressText);
		}
		if ((In.buttons & wire::BUTTON_GRENADE) != 0)
		{
			// 겉모습 수류탄이 보이기까지(G-5-b SpawnGrenadeVisual)를 재려고 단추를 실은 입력 번호 · 프레임을 둔다
			GrenadePressSeqs[GrenadePressRing % 8] = In.seq;
			GrenadePressFrames[GrenadePressRing % 8] = GFrameCounter;
			++GrenadePressRing;
		}
		if ((In.buttons & wire::BUTTON_GRENADE) != 0 && GrenadePressLinesLeft > 0)
		{
			--GrenadePressLinesLeft;
			UE_LOG(LogMOB, Log, TEXT("MOB_GRENADE_PRESS seq=%u frame=%llu camBack=%.2f ads=%d crouch=%d pitch=%.2f thrown=%d"), In.seq, (unsigned long long)GFrameCounter, CamBack, bAds ? 1 : 0, bSweepCrouchNow ? 1 : 0, In.pitch,
				Core->Prediction.GetGrenadeCounters().thrown != GrenadeThrownBefore ? 1 : 0);
		}
		// 대시(D-5) — 예측이 이 입력에 대시를 켰으면 Lyra 대시 능력은 TickAbilityDash 가 켠다. 방향 = 예측이 고른 방향(그 칸의 dashDir)
		if (Core->Prediction.GetDashCounters().started != DashStartedBefore)
		{
			wire::PlayerState Probe = wire::PlayerState();
			Probe.lastSeq = In.seq;
			const mo::CSelfPrediction::Slot* DashSlot = Core->Prediction.FindComparable(Probe);
			if (bDashPending) ++DashDropped;	// 앞 대시를 아직 못 켰다
			bDashPending = true;
			DashPendingSeq = In.seq;
			DashPendingFrame = GFrameCounter;
			DashPendingDir = DashSlot ? DashSlot->dashDir : 0xFF;	// 0xFF = 칸을 못 찾음 — 몽타주 대조에서 뺀다
			// 앞 입력 이동 = Lyra 날 값(길이 그대로 — 효과의 방향 값이 원본과 같게). 다만 예측이 쓴 앞 칸 걸음 이동(부활 뒤 이동 막음이면 0)과 고르는 방향이
			// 다르면 그 값을 넣는다 — 몽타주는 서버 · 예측 방향을 따른다(판 dsh2 seq 2388: 날 값 (-1, 1) · 예측 0 → 오른 ↔ 앞)
			DashPendingPrevMove = LastInputRawMove;
			bDashPendingPrevYaw = false;
			wire::PlayerState PrevProbe = wire::PlayerState();
			PrevProbe.lastSeq = In.seq - 1;
			if (const mo::CSelfPrediction::Slot* PrevSlot = Core->Prediction.FindComparable(PrevProbe))
			{
				DashPendingPrevYaw = PrevSlot->input.yaw;
				bDashPendingPrevYaw = true;
				const mo::Vec3 Used(PrevSlot->dashMove.x, PrevSlot->dashMove.y, 0);
				if (mo::SelectDashDir(Used, PrevSlot->input.yaw) != mo::SelectDashDir(mo::Vec3(LastInputRawMove.X, LastInputRawMove.Y, 0), PrevSlot->input.yaw))
				{
					DashPendingPrevMove = FVector(Used.x, Used.y, 0);
					++DashPrevFromSlot;
				}
			}
			++DashQueued;
			if (DashPendingDir < 4) ++DashDirCount[DashPendingDir];
			DashStartSeqs[DashStartNext] = In.seq;
			DashStartNext = (DashStartNext + 1) % UE_ARRAY_COUNT(DashStartSeqs);
		}
		LastInputRawMove = RawMove;
		// 수류탄(G-5-a) — 예측이 이 입력에 던졌으면 끊기 · 몽타주 · 쿨다운 메시지는 TickAbilityGrenade 가 낸다(같은 프레임)
		if (Core->Prediction.GetGrenadeCounters().thrown != GrenadeThrownBefore)
		{
			if (bGrenadePending) ++GrenadeDropped;	// 앞 던짐을 아직 못 냈다 — 쿨다운(301입력)이 한 프레임보다 길어 안 나와야 한다
			bGrenadePending = true;
			GrenadePendingSeq = In.seq;
			GrenadePendingFrame = GFrameCounter;
			++GrenadeQueued;
		}
		if (bMeleeStarts)
		{
			MeleeGuesses.Add(In.seq, MeleeGuess);
			if (bMeleePending) ++MeleeDropped;	// 앞 근접을 아직 못 켰다 — 능력 간격보다 오래 못 켠 것
			bMeleePending = true;
			MeleePendingSeq = In.seq;
			MeleePendingFrame = GFrameCounter;
			++MeleeQueued;
			// 각본 근접 칸이면 시작 입력 번호를 무기별로 — 누른 채 다시 켜지는 간격(입력 수)을 Lyra 기록 34 · 35 · 46 프레임과 견준다
			const uint8 MeleeWeapon = (uint8)Core->Prediction.GetWeaponIndex();
			if (bHumanCheck && HcStep < kHumanStepCount && kHumanSteps[HcStep].Act == EHcAct::MeleeHold && MeleeWeapon < 3) HcMeleeSeqs[MeleeWeapon].Add(In.seq);
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

	// 무기 각본의 시험 자리 옮기기(W-4) — 이 프레임 입력 「뒤」에 보낸다. 서버는 옮기기를 입력 줄에 같이 세워 차례대로 쓰고(TickPlayerInputs) 한도를 안 쓴다 —
	// 입력 앞에 두면 앞 틱 마지막 입력과 같은 틱에 쓰인다(프로브 RunWeapons 와 같은 까닭). 번호는 예측이 하나 잡고 서버 ApplyPlace 를 따라 한다(OnPlace)
	if (bWsPlacePending)
	{
		bWsPlacePending = false;
		wire::DebugPlace Place = wire::DebugPlace();
		Place.h.size = sizeof(Place);
		Place.h.type = (uint16)wire::Msg::C2S_DEBUG_PLACE;
		const bool bSetAmmo = (WsPlaceFlags & wire::DEBUG_PLACE_SET_AMMO) != 0;
		{
			const FMOBCore::FPawnScope PawnScope(*Core);
			Place.seq = Core->Prediction.OnPlace(WsPlaceAt.X, WsPlaceAt.Y, WsPlaceAt.Z, bSetAmmo ? WsAmmoWeapon : (uint8)0xFF, WsAmmoMag, WsAmmoSpare);
		}
		Seq = Place.seq;
		Place.x = WsPlaceAt.X;
		Place.y = WsPlaceAt.Y;
		Place.z = WsPlaceAt.Z;
		Place.hp = ServerHp > 0.f ? ServerHp : 100.f;	// 체력은 그대로 — 서버 ApplyPlace 가 이 값으로 놓는다
		Place.flags = WsPlaceFlags;
		Place.ammoWeapon = WsAmmoWeapon;
		Place.magazine = WsAmmoMag;
		Place.spare = WsAmmoSpare;
		QueuePacket(&Place, sizeof(Place));
		++WsPlaces;
		// Lyra 쪽 같은 탄 정하기 — 빠른 칸의 그 무기 아이템. 든 무기면 다음 상태가 어차피 맞추고, 안 든 무기는 다음 줍기(탄 채움)의 앞 값이 된다
		if (bSetAmmo) SetLyraItemAmmo(WsAmmoWeapon, WsAmmoMag, WsAmmoSpare);
		SelfCur = ToFVector(Core->Prediction.GetMove().location);
		SelfPrev = SelfCur;
		VisualError = FVector::ZeroVector;
		UE_LOG(LogMOB, Log, TEXT("MOB_WS_PLACE step=%d seq=%u at=(%.1f,%.1f,%.1f) flags=%u ammo=%u,%d,%d"), WsStep, Place.seq, Place.x, Place.y, Place.z, Place.flags, Place.ammoWeapon, (int32)Place.magazine, (int32)Place.spare);
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
		if (M.target == PlayerId && M.result == wire::MELEE_HIT) LastMeleeOnMeAt = FPlatformTime::Seconds();	// 남이 나를 밀었다 — 내 예측은 모르는 밀기(MOB_CORR)
		return;
	}
	const bool bHit = M.result == wire::MELEE_HIT;
	if (bHit) LastMyMeleeHitAt = FPlatformTime::Seconds();
	if (bHit && bHumanCheck)
	{
		++HcMeleeHits;
		HcMeleeTarget = M.target;
	}
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

// 서버 대시 소식(D-3) — 내 것이면 예측을 서버 값으로 고친다(다르면 그 입력부터 대시를 다시 고르고 걸음을 다시 돌린다). 남의 것은 세기만 — 남의 몸 효과는 D-5
void UMOBridgeSubsystem::HandleDash(const wire::Dash& M)
{
	if (M.player != PlayerId)
	{
		// 남의 대시(D-5) — 몽타주는 서버 애니 상태가 낸다(-MOBServerAnim). 효과는 그 몸이 그 틱 상태로 넘어갈 때 낸다(TickRemoteDashCues) —
		// 받자마자 내면 화면의 남 몸(보간 지연만큼 뒤)이 아직 안 움직였는데 효과가 먼저 나온다
		++DashOthers;
		if (PendingDashCues.Num() >= 32)
		{
			PendingDashCues.RemoveAt(0);	// 화면 시계가 안 가는 동안 쌓인 것 — 32 = 받고 낼 때까지(판 dsh1 · dsh2 최대 0.37초) 쌓일 수보다 넉넉한 상한(제 판단)
			++DashCueDropped;
		}
		FMOBPendingDashCue& Cue = PendingDashCues.AddDefaulted_GetRef();
		Cue.Player = M.player;
		Cue.Tick = M.tick;
		Cue.Force = FVector(M.forceX, M.forceY, M.forceZ);
		Cue.ReceivedAt = FPlatformTime::Seconds();
		return;
	}
	++DashMine;
	if (Core)
	{
		const FMOBCore::FPawnScope PawnScope(*Core);	// 그 입력부터 다시 돌리는 걸음도 남 캡슐을 본다(근접과 같다)
		Core->Prediction.OnDashResult(M.seq, M.dir, mo::Vec3(M.forceX, M.forceY, M.forceZ));
	}
}

// 서버 수류탄 소식(G-5-a) — 내 던짐이면 예측을 서버 값으로 고친다(그 입력이 안 던졌으면 던짐으로 놓고 뒤 칸의 쿨다운 · 던짐을 다시 고른다 — 봇 OnGrenade 와 같은 길).
// 던짐 · 터짐은 겉모습 수류탄 칸에 틱 하나로 적는다(G-5-b — 만들기 · 터짐 신호는 TickGrenadeVisuals 가 화면 시점에)
void UMOBridgeSubsystem::HandleGrenade(const wire::Grenade& M)
{
	switch (M.kind)
	{
	case wire::GRENADE_THROW:
		break;
	case wire::GRENADE_BOOM_TIMER:
	case wire::GRENADE_BOOM_HIT:
	{
		++GrenadeBooms;
		FMOBGrenadeVisual* G = GrenadeVisuals.Find(M.id);
		if (!G || G->bBoomKnown)
		{
			++GrenadeBoomUnknown;
			return;
		}
		G->bBoomKnown = true;
		if (Core)
		{
			// 걸음 몸(L-8-b-2) — 새 입력부터 안 본다. 다시 돌리지 않는다: 터진 틱의 입력은 터지기 전 몸을 봤다(Room::Tick 차례: 입력 → 수류탄)
			const uint32_t Object = mo::kMapExtraObjectBit | M.id;
			Core->GrenadeBodies.erase(std::remove_if(Core->GrenadeBodies.begin(), Core->GrenadeBodies.end(), [Object](const mo::MapCapsule& C) { return C.object == Object; }), Core->GrenadeBodies.end());
		}
		G->BoomTick = M.tick;
		G->BoomKind = M.kind;
		// 터진 틱 걸음 뒤 자리 — 그 틱 상태 칸에는 안 든다(서버가 목록에서 뺐다, GameProtocol.h GrenadeStateHead)
		if (G->Snaps.Num() == 0 || G->Snaps.Last().Tick < M.tick)
		{
			FMOBGrenadeSnap& S = G->Snaps.AddDefaulted_GetRef();
			S.Tick = M.tick;
			S.Pos = FVector(M.x, M.y, M.z);
			S.Rot = FRotator(M.pitch, M.yaw, M.roll);
		}
		else ++GrenadeStateOld;
		return;
	}
	default:
		++GrenadeBadKind;
		return;
	}
	// 겉모습 수류탄 칸 — 번호 0 은 목록이 차 서버도 수류탄을 못 만든 던짐(만들 것 없음)
	if (M.id == 0) ++GrenadeThrowsNoId;
	else if (GrenadeVisuals.Contains(M.id)) ++GrenadeDupId;
	else
	{
		++GrenadeThrowsWithId;
		FMOBGrenadeVisual& G = GrenadeVisuals.Add(M.id);
		G.Id = M.id;
		G.Thrower = M.thrower;
		G.ThrowTick = M.tick;
		G.Seq = M.seq;
		G.Snaps.Reserve(128);
		// 생긴 자리는 던진 틱 걸음 앞(서버는 입력 처리 안에서 만들고 같은 틱 TickGrenades 에서 한 걸음 — Room.cpp ThrowGrenade · TickGrenades) = 던진 틱 − 1 칸.
		// 던진 사람 몸도 그 입력 걸음 앞 자리에서 던지므로 남 몸 보간의 같은 칸과 맞는다
		FMOBGrenadeSnap& S = G.Snaps.AddDefaulted_GetRef();
		S.Tick = M.tick > 0 ? M.tick - 1 : 0;
		S.Pos = FVector(M.x, M.y, M.z);
		S.Rot = FRotator(M.pitch, M.yaw, M.roll);
	}
	if (M.thrower != PlayerId)
	{
		++GrenadeOthers;
		return;
	}
	++GrenadeMine;
	if (Core)
	{
		const FMOBCore::FPawnScope PawnScope(*Core);	// 근접 끊음이 바뀌어 다시 돌리는 걸음도 남 캡슐을 본다(봇과 같다)
		Core->Prediction.OnGrenadeThrown(M.seq);
	}
}

// 서버 수류탄 상태(G-5-b) — 머리 뒤 칸 count 개. 칸마다 그 번호 겉모습 수류탄의 틱 하나(걸음 뒤 자리 · 회전 · 튕김)로 쌓는다
void UMOBridgeSubsystem::HandleGrenadeState(const uint8* Data, int32 Size)
{
	wire::GrenadeStateHead H = wire::GrenadeStateHead();
	if (Size < (int32)sizeof(H))
	{
		++GrenadeStateBadSize;
		return;
	}
	FMemory::Memcpy(&H, Data, sizeof(H));
	if (Size < (int32)(sizeof(H) + sizeof(wire::GrenadeStateEntry) * H.count))
	{
		++GrenadeStateBadSize;
		return;
	}
	// 걸음 몸(L-8-b-2) — 이 소식이 그 틱 끝 산 수류탄 전부. 칸 값 그대로(서버도 와이어 폭으로 줄여 만든다) · 제 수류탄은 던진 틱으로 0.16초 서로 무시를 셈
	if (Core)
	{
		Core->GrenadeBodies.clear();
		Core->GrenadeListTick = H.tick;
		Core->bGrenadeListOpen = true;
		++GrenadeWalkLists;
		const mo::ProjectileParams Params{};
		const float TickSeconds = 1.f / (float)wire::kTickRate;	// 서버 kTickSeconds 와 같은 식
		for (int32 i = 0; i < (int32)H.count; ++i)
		{
			wire::GrenadeStateEntry E = wire::GrenadeStateEntry();
			FMemory::Memcpy(&E, Data + sizeof(H) + sizeof(E) * i, sizeof(E));
			const FMOBGrenadeVisual* Thrown = GrenadeVisuals.Find(E.id);
			if (Thrown && Thrown->Thrower == PlayerId && mo::GrenadeIgnoresThrower(H.tick, Thrown->ThrowTick, TickSeconds, Params))
			{
				++GrenadeWalkOwnIgnored;
				continue;
			}
			mo::Rotator Rotation;
			Rotation.pitch = E.pitch;
			Rotation.yaw = E.yaw;
			Rotation.roll = E.roll;
			Core->GrenadeBodies.push_back(mo::GrenadeStandable(mo::Vec3(E.x, E.y, E.z), Rotation, Params, E.id));
		}
		GrenadeWalkBodies += (int64)Core->GrenadeBodies.size();
		if (Core->RerunForGrenadeBodies()) ++GrenadeWalkReruns;
		else ++GrenadeWalkRerunsNone;
	}
	for (int32 i = 0; i < (int32)H.count; ++i)
	{
		wire::GrenadeStateEntry E = wire::GrenadeStateEntry();
		FMemory::Memcpy(&E, Data + sizeof(H) + sizeof(E) * i, sizeof(E));
		FMOBGrenadeVisual* G = GrenadeVisuals.Find(E.id);
		if (!G)
		{
			++GrenadeStateUnknown;
			continue;
		}
		if (G->Snaps.Num() > 0 && G->Snaps.Last().Tick >= H.tick)
		{
			++GrenadeStateOld;
			continue;
		}
		++GrenadeStateEntries;
		FMOBGrenadeSnap& S = G->Snaps.AddDefaulted_GetRef();
		S.Tick = H.tick;
		S.Pos = FVector(E.x, E.y, E.z);
		S.Rot = FRotator(E.pitch, E.yaw, E.roll);
		if ((E.flags & wire::GRENADE_STATE_BOUNCED) != 0)
		{
			++GrenadeStateBounced;
			S.bBounced = true;
			S.Hit = FVector(E.hitX, E.hitY, E.hitZ);
		}
	}
}

// 겉모습 수류탄 만들기(G-5-b) — 능력과 같은 클래스 · 생성 값(AlwaysSpawn · 루트 크기 1 · 던진 몸 = Instigator, 원본 기준 ga_flow.txt:2-10)으로 만든다.
// 만들면 BeginPlay 가 꼬리(던진 몸의 팀 색)를 붙이고 타이머 둘(터짐 TimeBeforeExplode 2.0 · 0.16초 뒤 PostLaunchCleanup = 던진 몸과 서로 무시를 풂)을 건다(b_flow.txt:69-114 · 259-302) —
// 타이머를 다 지우고 날아감을 끄고 충돌을 끈다. 충돌을 끄는 까닭: 메시는 사람 캡슐을 막고(G-0-c2 GCOLL) 다른 팀 몸이 닿으면 ReceiveHit 가 Detonate 를 부른다(b_flow.txt:115-132) —
// 독립 실행이라 그 Detonate 가 Lyra 쪽 피해를 넣는다. 수류탄 충돌 · 터짐은 서버 몫이다
void UMOBridgeSubsystem::SpawnGrenadeVisual(FMOBGrenadeVisual& G)
{
	G.bSpawned = true;
	UWorld* World = GetWorld();
	// 첫 겉모습 앞에 신호 길 알림을 건다(그 뒤 터짐 신호를 센다 — 블루프린트 터짐 타이머 2.0초보다 앞이다)
	if (!GrenadeCueRouteHandle.IsValid())
	{
		if (UGameplayCueManager* GCM = UAbilitySystemGlobals::Get().GetGameplayCueManager())
		{
			const FGameplayTag CueTag = FGameplayTag::RequestGameplayTag(kGrenadeDetonateCueTag, false);
			GrenadeCueRouteHandle = GCM->OnGameplayCueRouted().AddWeakLambda(this, [this, CueTag](AActor*, FGameplayTag Routed, EGameplayCueEvent::Type Event, const FGameplayCueParameters&, EGameplayCueExecutionOptions)
			{
				if (Event != EGameplayCueEvent::Executed || !CueTag.IsValid() || Routed != CueTag) return;
				if (bGrenadeCueCalling) ++GrenadeCueRoutedMine;
				else ++GrenadeCueRoutedOther;
			});
		}
	}
	UClass* GrenadeClass = LoadClass<AActor>(nullptr, kGrenadeActorClassPath);
	const FMOBGrenadeSnap& S0 = G.Snaps[0];
	APawn* InstigatorPawn = FindPlayerPawn(G.Thrower);
	if (!InstigatorPawn) ++GrenadeVisNoInstigator;	// 꼬리 팀 색은 블루프린트가 Instigator 팀으로 고른다 — 없으면 팀 1 이 아닌 쪽 색
	FActorSpawnParameters P;
	P.Instigator = InstigatorPawn;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	P.TransformScaleMethod = ESpawnActorScaleMethod::OverrideRootScale;
	AActor* A = (World && GrenadeClass) ? World->SpawnActor<AActor>(GrenadeClass, FTransform(S0.Rot, S0.Pos, FVector::OneVector), P) : nullptr;
	if (!A)
	{
		++GrenadeVisSpawnFail;
		UE_LOG(LogMOB, Warning, TEXT("MOB_GRENADE_VIS spawnFail id=%u class=%s"), G.Id, *GetNameSafe(GrenadeClass));
		return;
	}
	G.Actor = A;
	G.SpawnFrame = GFrameCounter;
	G.SpawnTime = World->GetTimeSeconds();
	++GrenadeVisSpawned;

	// 터짐 타이머 손잡이(블루프린트 변수 Timer) — 지우기 전에 켜져 있었나 · 지운 뒤 꺼졌나를 본다. 칸 형이 FTimerHandle 일 때만 그 꼴로 읽는다
	FTimerManager& TM = World->GetTimerManager();
	FTimerHandle DetonateTimer;
	if (const FStructProperty* TP = CastField<FStructProperty>(A->GetClass()->FindPropertyByName(TEXT("Timer"))))
	{
		if (TP->Struct && TP->Struct->GetFName() == FName(TEXT("TimerHandle")) && TP->Struct->GetStructureSize() == (int32)sizeof(FTimerHandle))
		{
			DetonateTimer = *TP->ContainerPtrToValuePtr<FTimerHandle>(A);
		}
	}
	if (DetonateTimer.IsValid() && TM.IsTimerActive(DetonateTimer)) ++GrenadeVisTimersActive;
	if (!bBreakGrenadeVisual) TM.ClearAllTimersForObject(A);
	if (DetonateTimer.IsValid() && TM.IsTimerActive(DetonateTimer)) ++GrenadeVisTimersLeft;
	if (UProjectileMovementComponent* PMC = A->FindComponentByClass<UProjectileMovementComponent>())
	{
		PMC->StopMovementImmediately();
		PMC->Deactivate();
	}
	else ++GrenadeVisNoMovement;
	A->SetActorEnableCollision(false);

	if (const FObjectProperty* MP = CastField<FObjectProperty>(A->GetClass()->FindPropertyByName(TEXT("Mesh"))))
	{
		G.Mesh = Cast<UPrimitiveComponent>(MP->GetObjectPropertyValue_InContainer(A));
	}
	if (!G.Mesh.IsValid()) G.Mesh = Cast<UPrimitiveComponent>(A->GetRootComponent());
	if (const FObjectProperty* NP = CastField<FObjectProperty>(A->GetClass()->FindPropertyByName(TEXT("NS_Trail"))))
	{
		G.Trail = Cast<UActorComponent>(NP->GetObjectPropertyValue_InContainer(A));
	}

	// 내 수류탄 — Q 를 누른 프레임에서 보이기까지(서버 왕복 + 남 몸과 같은 화면 늦춤)
	int64 ShowDelay = -1;
	if (G.Thrower == PlayerId)
	{
		for (int32 k = 0; k < 8; ++k)
		{
			if (GrenadePressFrames[k] != 0 && GrenadePressSeqs[k] == G.Seq) ShowDelay = (int64)(GFrameCounter - GrenadePressFrames[k]);
		}
		if (ShowDelay < 0) ++GrenadeShowNoPress;
		else
		{
			++GrenadeShowDelayN;
			GrenadeShowDelaySum += ShowDelay;
			GrenadeShowDelayMin = FMath::Min(GrenadeShowDelayMin, ShowDelay);
			GrenadeShowDelayMax = FMath::Max(GrenadeShowDelayMax, ShowDelay);
		}
	}
	if (GrenadeVisLinesLeft > 0)
	{
		--GrenadeVisLinesLeft;
		UE_LOG(LogMOB, Log, TEXT("MOB_GRENADE_VIS spawn id=%u thrower=%u mine=%d throwTick=%u render=%.3f frame=%llu instigator=%s trail=%d mesh=%d showDelay=%lld"),
			G.Id, G.Thrower, G.Thrower == PlayerId ? 1 : 0, G.ThrowTick, ShownRenderTick, (unsigned long long)GFrameCounter, *GetNameSafe(InstigatorPawn), G.Trail.IsValid() ? 1 : 0, G.Mesh.IsValid() ? 1 : 0, ShowDelay);
	}
}

// 겉모습 수류탄(G-5-b) — 사건 틱 X 는 화면 시점이 X − 1 을 넘은 첫 프레임에 낸다(남의 대시 효과 TickRemoteDashCues 와 같은 규칙 — 서버가 틱 X 처리 안에서 일으켰다).
// 만들기(던진 틱) → 자리 · 회전 보간(남 몸과 같은 ShownRenderTick) → 튕김 소리(상태 칸의 틱) → 터짐(터진 틱 — 그 자리로 옮기고 블루프린트 Detonate 의 화면 몫:
// 신호 · 메시 숨김, b_flow.txt:31-50 · 151-154 · 186-189) → 꼬리가 끝나면 지움(블루프린트는 꼬리 OnSystemFinished 에 지운다, b_flow.txt:16-17 · 146-150)
void UMOBridgeSubsystem::TickGrenadeVisuals()
{
	if (GrenadeVisuals.Num() == 0 || ShownRenderTick < 1.0) return;
	static const FGameplayTag DetonateTag = FGameplayTag::RequestGameplayTag(kGrenadeDetonateCueTag, false);
	const double R = ShownRenderTick;
	UWorld* World = GetWorld();
	USoundBase* ImpactSound = nullptr;
	USoundAttenuation* ImpactAttenuation = nullptr;
	bool bSoundLoaded = false;
	// 블루프린트 ReceiveHit 의 소리와 같은 값(b_flow.txt:131-139 — 맞은 자리 · 크기 1 · 높이 1 · 시작 0 · 감쇠 ATT_Projectile · 끝나면 지움)
	const auto PlayImpact = [&](const FVector& At)
	{
		if (!bSoundLoaded)
		{
			bSoundLoaded = true;
			ImpactSound = LoadObject<USoundBase>(nullptr, kGrenadeImpactSoundPath);
			ImpactAttenuation = LoadObject<USoundAttenuation>(nullptr, kGrenadeImpactAttenuationPath);
		}
		if (ImpactSound) UGameplayStatics::SpawnSoundAtLocation(this, ImpactSound, At, FRotator::ZeroRotator, 1.f, 1.f, 0.f, ImpactAttenuation, nullptr, true);
	};

	for (auto It = GrenadeVisuals.CreateIterator(); It; ++It)
	{
		FMOBGrenadeVisual& G = It.Value();
		if (!G.bSpawned && R > (double)G.ThrowTick - 1.0) SpawnGrenadeVisual(G);
		if (!G.bSpawned) continue;
		if (G.SpawnFrame == 0)
		{
			if (G.bBoomKnown) It.RemoveCurrent();	// 만들기 실패 — 터짐 소식까지 받고 뺀다(뒤 소식이 「칸 없음」으로 안 세이게)
			continue;
		}
		AActor* A = G.Actor.Get();
		if (!A)
		{
			if (!G.bDestroyedByUs)
			{
				++GrenadeVisLost;
				// 일부러 틀림(-MOBBreakGrenadeVisual)에선 블루프린트가 스스로 터지고 꼬리가 끝나면 스스로 지운다 — 그 지움까지 초를 다리 지움과 같은 꼴로 남겨
				// 같은 판 조건(느린 프레임)에서 Lyra 원본 길과 견준다. 알아챈 프레임은 지워진 다음 프레임이다
				if (GrenadeGoneLinesLeft > 0)
				{
					--GrenadeGoneLinesLeft;
					UE_LOG(LogMOB, Log, TEXT("MOB_GRENADE_VIS lostGone id=%u frame=%llu fromSpawn=%lld sec=%.3f maxDt=%.3f overDt=%.3f spawnDt=%.3f selfHidden=%d"), G.Id, (unsigned long long)GFrameCounter,
						(int64)(GFrameCounter - G.SpawnFrame), World->GetTimeSeconds() - G.SpawnTime, G.MaxDt, G.OverDt, G.SpawnDt, G.bSelfHidden ? 1 : 0);
				}
			}
			It.RemoveCurrent();
			continue;
		}
		if (GFrameCounter == G.SpawnFrame) G.SpawnDt = World->GetDeltaSeconds();
		else
		{
			G.MaxDt = FMath::Max(G.MaxDt, World->GetDeltaSeconds());
			G.OverDt += FMath::Max(World->GetDeltaSeconds() - 0.125f, 0.f);
		}
		UPrimitiveComponent* Mesh = G.Mesh.Get();
		if (!G.bBoomed && !G.bSelfHidden && Mesh && Mesh->bHiddenInGame)
		{
			// 다리가 숨기기 전에 메시가 숨었다 = 블루프린트 Detonate 가 돌았다(그 첫 화면 일이 메시 숨김이다)
			G.bSelfHidden = true;
			++LocalGrenadeDetonate;
			UE_LOG(LogMOB, Warning, TEXT("MOB_GRENADE_VIS localDetonate id=%u frame=%llu age=%.3f"), G.Id, (unsigned long long)GFrameCounter, World->GetTimeSeconds() - G.SpawnTime);
		}

		for (const FMOBGrenadeSnap& S : G.Snaps)
		{
			if (!S.bBounced || S.Tick <= G.LastSoundTick || R <= (double)S.Tick - 1.0) continue;
			G.LastSoundTick = S.Tick;
			PlayImpact(S.Hit);
			++GrenadeVisBounceSounds;
		}

		if (!G.bBoomed)
		{
			// 보간 — 화면 시점 R 이하의 마지막 틱과 그다음 틱을 섞는다(틱 사이가 비면 그 폭으로). 다음 틱이 아직 없으면 마지막 틱에 둔다
			const TArray<FMOBGrenadeSnap>& Snaps = G.Snaps;
			int32 i = Snaps.Num() - 1;
			while (i > 0 && (double)Snaps[i].Tick > R) --i;
			FVector Pos = Snaps[i].Pos;
			FRotator Rot = Snaps[i].Rot;
			if (i + 1 < Snaps.Num())
			{
				const FMOBGrenadeSnap& From = Snaps[i];
				const FMOBGrenadeSnap& To = Snaps[i + 1];
				const double Span = FMath::Max<double>((double)To.Tick - (double)From.Tick, 1.0);
				const float Alpha = (float)FMath::Clamp((R - (double)From.Tick) / Span, 0.0, 1.0);
				Pos = FMath::Lerp(From.Pos, To.Pos, Alpha);
				Rot = FQuat::Slerp(From.Rot.Quaternion(), To.Rot.Quaternion(), Alpha).Rotator();
			}
			else if ((double)Snaps[i].Tick < R) ++GrenadeVisUnderrun;
			A->SetActorLocationAndRotation(Pos, Rot, false, nullptr, ETeleportType::None);
		}

		if (G.bBoomKnown && !G.bBoomed && !bBreakGrenadeVisual && R > (double)G.BoomTick - 1.0)
		{
			G.bBoomed = true;
			const FMOBGrenadeSnap& Boom = G.Snaps.Last();
			A->SetActorLocationAndRotation(Boom.Pos, Boom.Rot, false, nullptr, ETeleportType::None);
			// 블루프린트와 같은 신호 값(b_flow.txt:32-37 — 자리 = 수류탄 자리 · 법선 0 · 효과 · 능력 단계 1, 나머지 핀은 기본값)
			const FGameplayCueParameters Params = UAbilitySystemBlueprintLibrary::MakeGameplayCueParameters(0.f, 0.f, FGameplayEffectContextHandle(), FGameplayTag(), FGameplayTag(),
				FGameplayTagContainer(), FGameplayTagContainer(), A->GetActorLocation(), FVector::ZeroVector, nullptr, nullptr, nullptr, nullptr, 1, 1, nullptr, false);
			if (DetonateTag.IsValid())
			{
				bGrenadeCueCalling = true;	// 신호 길 알림은 이 부름 안에서 바로 온다(능력 시스템 없는 대상 → ExecuteGameplayCue_NonReplicated → RouteGameplayCue)
				UGameplayCueFunctionLibrary::ExecuteGameplayCueOnActor(A, DetonateTag, Params);
				bGrenadeCueCalling = false;
			}
			else ++GrenadeVisCueNoTag;
			if (Mesh) Mesh->SetHiddenInGame(true);
			++GrenadeVisBooms;
			// 다른 팀 몸에 맞아 터지면 블루프린트는 Detonate 뒤 맞은 자리에 부딪힘 소리를 낸다(b_flow.txt:131-132) — 맞은 자리는 와이어에 없어 터진 자리로
			if (G.BoomKind == wire::GRENADE_BOOM_HIT)
			{
				PlayImpact(Boom.Pos);
				++GrenadeVisHitSounds;
			}
			G.BoomFrame = GFrameCounter;
			G.BoomTime = World->GetTimeSeconds();
			if (GrenadeVisLinesLeft > 0)
			{
				--GrenadeVisLinesLeft;
				UE_LOG(LogMOB, Log, TEXT("MOB_GRENADE_VIS boom id=%u kind=%u boomTick=%u render=%.3f frame=%llu ageFrames=%llu age=%.3f"), G.Id, (unsigned)G.BoomKind, G.BoomTick, R,
					(unsigned long long)GFrameCounter, (unsigned long long)(GFrameCounter - G.SpawnFrame), G.BoomTime - G.SpawnTime);
			}
		}

		if (G.bBoomed)
		{
			UActorComponent* Trail = G.Trail.Get();
			if (!Trail || !Trail->IsActive())	// Niagara 는 끝나면 켜짐을 끈다(NiagaraComponent.cpp OnSystemComplete 의 SetActiveFlag(false))
			{
				const int64 GoneFrames = (int64)(GFrameCounter - G.SpawnFrame);
				const double GoneSec = World->GetTimeSeconds() - G.SpawnTime;
				GrenadeGoneFramesMin = FMath::Min(GrenadeGoneFramesMin, GoneFrames);
				GrenadeGoneFramesMax = FMath::Max(GrenadeGoneFramesMax, GoneFrames);
				GrenadeGoneSecMin = FMath::Min(GrenadeGoneSecMin, GoneSec);
				GrenadeGoneSecMax = FMath::Max(GrenadeGoneSecMax, GoneSec);
				if (GrenadeGoneLinesLeft > 0)
				{
					--GrenadeGoneLinesLeft;
					UE_LOG(LogMOB, Log, TEXT("MOB_GRENADE_VIS gone id=%u frame=%llu fromSpawn=%lld fromBoom=%llu sec=%.3f boomSec=%.3f maxDt=%.3f overDt=%.3f spawnDt=%.3f trail=%d"), G.Id, (unsigned long long)GFrameCounter, GoneFrames,
						(unsigned long long)(GFrameCounter - G.BoomFrame), GoneSec, G.BoomTime - G.SpawnTime, G.MaxDt, G.OverDt, G.SpawnDt, Trail ? 1 : 0);
				}
				G.bDestroyedByUs = true;
				++GrenadeVisGone;
				A->Destroy();
				It.RemoveCurrent();
				continue;
			}
		}
	}
}

// 남의 대시 효과 — GA_Hero_Dash 가 켤 때 내는 신호와 같은 값(원본 기준: 신호 GameplayCue.Character.Dash · NormalizedMagnitude = RootMotionDuration 0.25 ·
// RawMagnitude = Strength 1850 · Normal = 방향 · Location 0 · 능력 · 효과 단계 1). 방향은 소식의 힘(단위 벡터 × 1850)을 정규화한 것 —
// 원본은 정규화 전 방향(앞이면 카메라 앞 × (1, 1, 1.2), 그 밖이면 이동 입력 날 값)이라 길이가 다를 수 있다. 시점 = 그 몸을 보간하는 화면 틱이 대시 틱의 앞 틱을 넘은 첫 프레임
void UMOBridgeSubsystem::TickRemoteDashCues()
{
	static const FGameplayTag DashCueTag = FGameplayTag::RequestGameplayTag(TEXT("GameplayCue.Character.Dash"), false);
	const double NowSec = FPlatformTime::Seconds();
	for (int32 i = 0; i < PendingDashCues.Num();)
	{
		const FMOBPendingDashCue& C = PendingDashCues[i];
		if (ShownRenderTick < 1.0 || ShownRenderTick <= (double)C.Tick - 1.0)
		{
			++i;
			continue;
		}
		APawn* Pawn = FindPlayerPawn(C.Player);
		UAbilitySystemComponent* ASC = FindASC(Pawn);
		if (!ASC || !DashCueTag.IsValid()) ++DashCueNoPawn;
		else
		{
			FGameplayCueParameters Params;
			Params.NormalizedMagnitude = mo::kDashForceDuration;
			Params.RawMagnitude = mo::kDashStrength;
			Params.Normal = C.Force.GetSafeNormal();
			Params.Location = FVector::ZeroVector;
			Params.AbilityLevel = 1;
			Params.GameplayEffectLevel = 1;
			ASC->ExecuteGameplayCue(DashCueTag, Params);
			++DashCues;
			const double Wait = NowSec - C.ReceivedAt;
			DashCueWaitSum += Wait;
			DashCueWaitMax = FMath::Max(DashCueWaitMax, Wait);
		}
		PendingDashCues.RemoveAt(i);
	}
}

// Lyra 대시 능력(GA_Hero_Dash)을 예측이 켠 대시 입력에서 켠다(근접 TickAbilityMelee 와 같은 꼴). 능력 칸의 입력 표식(InputTag.Ability.Dash)을 떼어
// Shift 로는 Lyra 가 혼자 못 켠다. 켠 능력이 내 몽타주 · 효과 · 소리 · 쿨다운 표시를 낸다. 힘은 Lyra 이동 계산이 쉬어(NoLyraMove) 내 자리를 안 바꾼다 — 자리는 예측이다.
// 방향: 능력은 켤 때 GetLastMovementInputVector 로 몽타주를 고른다. 사람 키면 Lyra 는 입력 처리(이동 계산 앞)에서 켜 앞 프레임 이동을 보지만, 다리는 이동 계산 뒤에 켜
// 이번 입력 이동을 본다 — 켜는 동안만 마지막 이동 입력을 앞 입력 이동(예측이 방향을 고른 값과 같은 쪽, CDashAbility::TryActivate · TickInput 의 DashPendingPrevMove)으로
// 바꿔 놓고 되돌린다(-MOBBreakDashDir 이면 안 바꾼다)
void UMOBridgeSubsystem::TickAbilityDash()
{
	static const FGameplayTag DashInputTag = FGameplayTag::RequestGameplayTag(TEXT("InputTag.Ability.Dash"), false);
	static const TCHAR* const MontNames[4] = { TEXT("AM_MM_Dash_Forward"), TEXT("AM_MM_Dash_Backward"), TEXT("AM_MM_Dash_Left"), TEXT("AM_MM_Dash_Right") };	// EDashDir 차례
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	ACharacter* Ch = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
	ULyraAbilitySystemComponent* LASC = Cast<ULyraAbilitySystemComponent>(FindASC(Ch));

	// 켠 다음 프레임에 내 메시에 있는 대시 몽타주 — 예측 방향의 몽타주여야 한다. 맨 위 몽타주만 보면 같은 프레임에 튼 다른 몽타주에 가린다
	// (판 dsh2 seq 1956: 빈 탄창으로 쥔 사격이 낸 AM_MM_Pistol_DryFire) — 섞여 빠지는 중인 것까지 몽타주 칸을 다 훑는다
	if (DashMontCheckFrame && GFrameCounter >= DashMontCheckFrame)
	{
		DashMontCheckFrame = 0;
		const UAnimInstance* AI = Ch && Ch->GetMesh() ? Ch->GetMesh()->GetAnimInstance() : nullptr;
		FString DashName, TopName(TEXT("none"));
		if (AI)
		{
			if (const UAnimMontage* Top = AI->GetCurrentActiveMontage()) TopName = Top->GetName();
			for (const FAnimMontageInstance* Inst : AI->MontageInstances)
			{
				const FString Name = Inst && Inst->Montage ? Inst->Montage->GetName() : FString();
				if (!Name.StartsWith(TEXT("AM_MM_Dash_"))) continue;
				DashName = Name;
				if (DashMontCheckDir < 4 && Name == MontNames[DashMontCheckDir]) break;
			}
		}
		++DashMontChecked;
		const bool bSame = DashMontCheckDir < 4 && DashName == MontNames[DashMontCheckDir];
		if (DashName.IsEmpty()) ++DashMontNone;
		else if (!bSame) ++DashMontageMismatch;
		if (!bSame && DashMontLinesLeft > 0)
		{
			--DashMontLinesLeft;
			UE_LOG(LogMOB, Log, TEXT("MOB_DASH_MONT seq=%u want=%d got=%s top=%s"), DashMontCheckSeq, (int32)DashMontCheckDir, DashName.IsEmpty() ? TEXT("none") : *DashName, *TopName);
		}
	}

	const FGameplayAbilitySpec* Spec = nullptr;
	if (LASC && DashInputTag.IsValid())
	{
		for (FGameplayAbilitySpec& Each : LASC->GetActivatableAbilities())
		{
			if (!Each.Ability || !Each.GetDynamicSpecSourceTags().HasTagExact(DashInputTag)) continue;
			if (!bBreakDashBlock || GDashSpecHandle != Each.Handle) UE_LOG(LogMOB, Log, TEXT("MOB_DASH_SPEC ability=%s handle=%s strip=%d"), *GetNameSafe(Each.Ability), *Each.Handle.ToString(), bBreakDashBlock ? 0 : 1);
			GDashSpecHandle = Each.Handle;
			if (!bBreakDashBlock)
			{
				Each.GetDynamicSpecSourceTags().RemoveTag(DashInputTag);
				LASC->MarkAbilitySpecDirty(Each);
				++LyraDashStripped;
			}
			break;
		}
		Spec = GDashSpecHandle.IsValid() ? LASC->FindAbilitySpecFromHandle(GDashSpecHandle) : nullptr;
	}
	const bool bActive = Spec && Spec->IsActive();
	if (bActive && !bLyraDashWasActive)
	{
		// 지난 프레임 끝엔 꺼져 있었는데 다리가 켜기 전에 켜졌다 = Lyra 가 혼자 켰다
		if (LyraDashSelf < 10) UE_LOG(LogMOB, Log, TEXT("MOB_DASH_LYRA_SELF frame=%llu seq=%u"), (unsigned long long)GFrameCounter, Seq);
		++LyraDashSelf;
		DashLastStartFrame = GFrameCounter;
	}
	if (bDashPending)
	{
		if (!Spec || !Ch)
		{
			bDashPending = false;
			++DashNoSpec;
		}
		else
		{
			bool bStarted = false;
			bool bSwapMattered = false;
			if (!bActive)
			{
				// 마지막 이동 입력만 바꾼다 — 이 프레임에 쌓인(다음 이동 계산이 가져갈) 입력은 그대로 남긴다
				const FVector Pending = Ch->Internal_GetPendingMovementInputVector();
				const FVector Last = Ch->Internal_GetLastMovementInputVector();
				// 예측은 앞 입력의 요(앞 입력 뒤의 몸 앞)로 고르는데 다리가 켜는 때 Lyra 몸은 이번 입력 뒤 요다 — 그 차이만큼 넣을 이동을 돌려 Lyra 가 예측과 같은
				// 각도 차(이동 − 몸 앞)를 보게 한다(판 dshd seq 780: 앞 입력 요 179.92 · 몸 -179.90 — 대각선 이동이 135도 경계에 걸려 예측 왼 ↔ Lyra 뒤).
				// 0 이동은 돌려도 0 이라 못 맞추지만, 그때 예측이 켜는 것은 앞(|요| < 45) 하나라 경계 45도에 걸릴 때만 갈린다
				const float BodyYaw = (float)Ch->GetActorRotation().Yaw;
				const FVector Swapped = bDashPendingPrevYaw
					? DashPendingPrevMove.RotateAngleAxis(FRotator::NormalizeAxis(BodyYaw - DashPendingPrevYaw), FVector::UpVector)
					: DashPendingPrevMove;
				const FVector Want = bBreakDashDir ? Last : Swapped;
				const auto SetLastInput = [Ch, Pending](const FVector& V)
				{
					Ch->Internal_ConsumeMovementInputVector();
					Ch->Internal_AddMovementInput(V, true);
					Ch->Internal_ConsumeMovementInputVector();
					Ch->Internal_AddMovementInput(Pending, true);
				};
				// 바꿔 넣기가 몽타주를 가르나 — 이번 입력 이동과 바꿔 넣을 값(돌린 앞 입력 이동)이 고르는 방향(몸 요 = 지금 몸)이 다르면
				bSwapMattered = mo::SelectDashDir(mo::Vec3(Last.X, Last.Y, 0), BodyYaw) != mo::SelectDashDir(mo::Vec3(Swapped.X, Swapped.Y, 0), BodyYaw);
				// 진단 — Lyra 가 고를 방향(넣을 이동 · 지금 몸 요)이 예측 방향과 다르면 예측이 쓴 값(앞 칸의 걸음 이동 · 입력 요)과 같이 남긴다
				const uint8 LyraWill = (uint8)mo::SelectDashDir(mo::Vec3(Want.X, Want.Y, 0), BodyYaw);
				if (LyraWill != DashPendingDir && DashMontLinesLeft > 0)
				{
					--DashMontLinesLeft;
					wire::PlayerState PrevProbe = wire::PlayerState();
					PrevProbe.lastSeq = DashPendingSeq - 1;
					const mo::CSelfPrediction::Slot* PrevSlot = Core ? Core->Prediction.FindComparable(PrevProbe) : nullptr;
					UE_LOG(LogMOB, Log, TEXT("MOB_DASH_DIRDIFF seq=%u want=%d lyraWill=%d put=(%.3f,%.3f) bodyYaw=%.2f last=(%.3f,%.3f) prevSlot=%d prevMove=(%.3f,%.3f) prevYaw=%.2f"),
						DashPendingSeq, (int32)DashPendingDir, (int32)LyraWill, Want.X, Want.Y, BodyYaw, Last.X, Last.Y, PrevSlot ? 1 : 0,
						PrevSlot ? PrevSlot->dashMove.x : 0.0, PrevSlot ? PrevSlot->dashMove.y : 0.0, PrevSlot ? PrevSlot->input.yaw : 0.f);
				}
				SetLastInput(Want);
				bStarted = LASC->TryActivateAbility(GDashSpecHandle);
				SetLastInput(Last);
			}
			if (bStarted)
			{
				bDashPending = false;
				++DashDriven;
				DashDirSwapMattered += bSwapMattered;
				DashLastStartFrame = GFrameCounter;
				DashMontCheckFrame = GFrameCounter + 1;
				DashMontCheckDir = DashPendingDir;
				DashMontCheckSeq = DashPendingSeq;
			}
			else
			{
				++DashWaitFrames;
				if (GFrameCounter - DashPendingFrame > kMaxDashWaitFrames)
				{
					// 끝까지 켜져 있었으면 버림(Lyra 가 먼저 켬 — -MOBBreakDashBlock), 꺼져 있는데 켜기가 안 됐으면 켜기 실패(쿨다운 등)
					bDashPending = false;
					if (bActive) ++DashDropped;
					else ++DashActivateFail;
				}
			}
		}
	}
	const FGameplayAbilitySpec* After = LASC && GDashSpecHandle.IsValid() ? LASC->FindAbilitySpecFromHandle(GDashSpecHandle) : nullptr;
	bLyraDashWasActive = After && After->IsActive();
}

// Lyra 수류탄 능력(GA_Grenade)은 다리가 켜지 않는다 — 독립 실행이라 권한 갈래에서 진짜 수류탄을 만들어 제 물리 · 타이머로 터뜨린다(grenade-plan 「화면(다리)」 · 결정 감사).
// 능력 칸의 입력 표식(InputTag.Weapon.Grenade)을 떼어 Q 로 Lyra 가 혼자 못 켜게 하고(근접 · 대시와 같은 꼴), 예측이 던진 입력에서 능력이 켤 때 화면에 내는 셋을 다리가 낸다 —
// ① 능력 무리 규칙의 끊기(재장전 · 감정 표현) ② 던짐 몽타주 ③ 쿨다운 메시지. 수류탄 액터는 서버 소식으로 G-5-b 가 만든다
void UMOBridgeSubsystem::TickAbilityGrenade()
{
	static const FGameplayTag GrenadeInputTag = FGameplayTag::RequestGameplayTag(TEXT("InputTag.Weapon.Grenade"), false);
	static const FGameplayTag GrenadeAbilityTag = FGameplayTag::RequestGameplayTag(TEXT("Ability.Type.Action.Grenade"), false);
	static const FGameplayTag ReloadAbilityTag = FGameplayTag::RequestGameplayTag(TEXT("Ability.Type.Action.Reload"), false);
	static const FGameplayTag DurationChannel = FGameplayTag::RequestGameplayTag(TEXT("Ability.Grenade.Duration.Message"), false);
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	ACharacter* Ch = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
	ULyraAbilitySystemComponent* LASC = Cast<ULyraAbilitySystemComponent>(FindASC(Ch));

	// Lyra 혼자 켬 — 이 능력은 켜진 그 프레임에 끝나 프레임마다 켜짐을 봐서는 못 잡는다. 능력 시스템의 켬 알림(PreActivate 가 부름)을 듣는다.
	// 능력 시스템은 선수 상태에 있어 부활해도 같지만, 바뀌면 옮겨 단다
	if (LASC && GrenadeWatchAsc.Get() != LASC)
	{
		if (UAbilitySystemComponent* Old = GrenadeWatchAsc.Get()) Old->AbilityActivatedCallbacks.Remove(GrenadeWatchHandle);
		GrenadeWatchHandle = LASC->AbilityActivatedCallbacks.AddUObject(this, &UMOBridgeSubsystem::OnLyraAbilityActivated);
		GrenadeWatchAsc = LASC;
	}

	const FGameplayAbilitySpec* Spec = nullptr;
	if (LASC && GrenadeInputTag.IsValid())
	{
		for (FGameplayAbilitySpec& Each : LASC->GetActivatableAbilities())
		{
			if (!Each.Ability || !Each.GetDynamicSpecSourceTags().HasTagExact(GrenadeInputTag)) continue;
			if (!bBreakGrenadeBlock || GGrenadeSpecHandle != Each.Handle) UE_LOG(LogMOB, Log, TEXT("MOB_GRENADE_SPEC ability=%s handle=%s strip=%d"), *GetNameSafe(Each.Ability), *Each.Handle.ToString(), bBreakGrenadeBlock ? 0 : 1);
			GGrenadeSpecHandle = Each.Handle;
			if (!bBreakGrenadeBlock)
			{
				Each.GetDynamicSpecSourceTags().RemoveTag(GrenadeInputTag);
				LASC->MarkAbilitySpecDirty(Each);
				++LyraGrenadeStripped;
			}
			break;
		}
		Spec = GGrenadeSpecHandle.IsValid() ? LASC->FindAbilitySpecFromHandle(GGrenadeSpecHandle) : nullptr;
	}
	if (!bGrenadePending) return;
	bGrenadePending = false;
	UAnimInstance* AI = Ch && Ch->GetMesh() ? Ch->GetMesh()->GetAnimInstance() : nullptr;
	if (!LASC || !AI)
	{
		++GrenadeNoSpec;	// 몸 · 애님 인스턴스 · 능력 시스템이 없다 — 화면에 낼 곳이 없다
		return;
	}

	// ① 끊기 — Lyra 는 능력을 켤 때 능력 표식을 무리 규칙(TagRelationshipMapping, TagRelationships_ShooterHero)으로 넓혀 그 표식을 단 켜진 능력을 끊는다
	// (LyraAbilitySystemComponent::ApplyAbilityBlockAndCancelTags → CancelAbilities). 그 함수는 보호, 넓히는 함수(GetAbilityTagsToBlockAndCancel)는 내보내지 않아
	// 다리에서 못 부르므로 같은 규칙 표를 반사로 읽어 넓히고 공개 CancelAbilities 를 부른다. 능력 자신의 끊기 표식(CancelAbilitiesWithTag)은 비어 있다(원본 기준 GA_Grenade_cdo.t3d).
	// 막기(켜진 동안 사격 · 재장전)는 안 한다 — Lyra 능력이 켜진 그 프레임에 끝나 막는 창이 없다(grenade-plan 「막기 · 끊기」, G-0-c 실측)
	static bool bCancelTagsLogged = false;
	FGameplayTagContainer CancelTags;
	{
		const FGameplayTagContainer AbilityTags = Spec && Spec->Ability ? Spec->Ability->GetAssetTags() : FGameplayTagContainer(GrenadeAbilityTag);
		const UObject* Mapping = nullptr;
		if (const FObjectProperty* MapProp = CastField<FObjectProperty>(LASC->GetClass()->FindPropertyByName(TEXT("TagRelationshipMapping"))))
		{
			Mapping = MapProp->GetObjectPropertyValue_InContainer(LASC);
		}
		const FArrayProperty* RelProp = Mapping ? CastField<FArrayProperty>(Mapping->GetClass()->FindPropertyByName(TEXT("AbilityTagRelationships"))) : nullptr;
		const FStructProperty* RelInner = RelProp ? CastField<FStructProperty>(RelProp->Inner) : nullptr;
		// 칸 형이 머리 파일의 것과 같을 때만 그 꼴로 읽는다
		if (RelInner && RelInner->Struct && RelInner->Struct->GetFName() == FName(TEXT("LyraAbilityTagRelationship")) && RelInner->Struct->GetStructureSize() == (int32)sizeof(FLyraAbilityTagRelationship))
		{
			const TArray<FLyraAbilityTagRelationship>& Rels = *RelProp->ContainerPtrToValuePtr<TArray<FLyraAbilityTagRelationship>>(Mapping);
			for (const FLyraAbilityTagRelationship& Rel : Rels)
			{
				if (AbilityTags.HasTag(Rel.AbilityTag)) CancelTags.AppendTags(Rel.AbilityTagsToCancel);
			}
		}
		if (!bCancelTagsLogged)
		{
			bCancelTagsLogged = true;
			UE_LOG(LogMOB, Log, TEXT("MOB_GRENADE_CANCELTAGS mapping=%s rels=%d abilityTags=%s cancel=%s"), *GetNameSafe(Mapping), RelProp && RelInner ? FScriptArrayHelper(RelProp, RelProp->ContainerPtrToValuePtr<void>(Mapping)).Num() : -1,
				*AbilityTags.ToStringSimple(), *CancelTags.ToStringSimple());
		}
	}
	int32 Cancelled = 0, CancelledReload = 0;
	FString CancelledNames;
	if (!CancelTags.IsEmpty())
	{
		// CancelAbilities 가 고르는 조건 그대로(켜짐 · 능력 표식이 끊기 표식 중 하나) 먼저 세고 끊는다
		for (const FGameplayAbilitySpec& Each : LASC->GetActivatableAbilities())
		{
			if (!Each.IsActive() || !Each.Ability || !Each.Ability->GetAssetTags().HasAny(CancelTags)) continue;
			++Cancelled;
			if (Each.Ability->GetAssetTags().HasTag(ReloadAbilityTag)) ++CancelledReload;
			CancelledNames += (CancelledNames.IsEmpty() ? TEXT("") : TEXT(",")) + GetNameSafe(Each.Ability);
		}
		LASC->CancelAbilities(&CancelTags, nullptr, nullptr);
		for (const FGameplayAbilitySpec& Each : LASC->GetActivatableAbilities())
		{
			if (Each.IsActive() && Each.Ability && Each.Ability->GetAssetTags().HasAny(CancelTags)) ++GrenadeCancelLeft;
		}
	}
	GrenadeCancelled += Cancelled;
	GrenadeCancelledReload += CancelledReload;

	// ② 몽타주 — 능력이 PlayMontageAndWait(속도 1 · 시작 0)로 트는 자산을 능력 클래스의 칸(GrenadeThrowMontage)에서 읽는다. 능력 시스템의 몽타주 틀기는
	// 능력 없이 부르는 쪽(PlayMontageSimulated)도 같은 Montage_Play 를 부르고 능력 시스템의 지금 몽타주 칸을 채운다
	UAnimMontage* Montage = nullptr;
	const TCHAR* MontageFrom = TEXT("spec");
	if (Spec && Spec->Ability)
	{
		if (const FObjectProperty* MontProp = CastField<FObjectProperty>(Spec->Ability->GetClass()->FindPropertyByName(TEXT("GrenadeThrowMontage"))))
		{
			Montage = Cast<UAnimMontage>(MontProp->GetObjectPropertyValue_InContainer(Spec->Ability));
		}
	}
	if (!Montage)
	{
		MontageFrom = TEXT("path");
		Montage = LoadObject<UAnimMontage>(nullptr, kGrenadeThrowMontagePath);
	}
	const float MontageLength = Montage ? LASC->PlayMontageSimulated(Montage, 1.f) : -1.f;
	if (MontageLength <= 0.f) ++GrenadeMontageFail;

	// ③ 쿨다운 메시지 — HUD W_GrenadeCooldown 이 듣는다(형 LyraInteractionDurationMessage = 던진 캐릭터 · 남은 시간, 원본 기준 ga_flow.txt:22-29 · :165-169)
	if (DurationChannel.IsValid())
	{
		FLyraInteractionDurationMessage Msg;
		Msg.Instigator = Ch;
		Msg.Duration = kLyraGrenadeCooldownSeconds;
		UGameplayMessageSubsystem::Get(this).BroadcastMessage(DurationChannel, Msg);
	}
	++GrenadeDriven;
	if (GrenadeLinesLeft > 0)
	{
		--GrenadeLinesLeft;
		UE_LOG(LogMOB, Log, TEXT("MOB_GRENADE_ONE seq=%u lastSeq=%u frame=%llu waitFrames=%llu cooldownLeft=%d montage=%s from=%s length=%.4f cancelled=%d reload=%d names=[%s]"),
			GrenadePendingSeq, Seq, (unsigned long long)GFrameCounter, (unsigned long long)(GFrameCounter - GrenadePendingFrame), Core ? Core->Prediction.GetGrenadeCooldownLeft() : -1,
			*GetNameSafe(Montage), MontageFrom, MontageLength, Cancelled, CancelledReload, *CancelledNames);
	}
}

void UMOBridgeSubsystem::OnLyraAbilityActivated(UGameplayAbility* Ability)
{
	static const FGameplayTag GrenadeAbilityTag = FGameplayTag::RequestGameplayTag(TEXT("Ability.Type.Action.Grenade"), false);
	if (!Ability || !GrenadeAbilityTag.IsValid() || !Ability->GetAssetTags().HasTagExact(GrenadeAbilityTag)) return;
	// 다리는 이 능력을 켜지 않으므로 켜짐은 모두 Lyra 혼자(-MOBBreakGrenadeBlock 이면 Q 로)
	if (LyraGrenadeSelf < 10) UE_LOG(LogMOB, Log, TEXT("MOB_GRENADE_LYRA_SELF frame=%llu seq=%u ability=%s"), (unsigned long long)GFrameCounter, Seq, *GetNameSafe(Ability));
	++LyraGrenadeSelf;
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
			if (LASC->TryActivateAbility(GMeleeSpecHandle))
			{
				++MeleeDriven;
				if (bHumanCheck)
				{
					const uint8 W = (uint8)Core->Prediction.GetWeaponIndex();
					if (HcStep < kHumanStepCount && kHumanSteps[HcStep].Act == EHcAct::MeleeHold && W < 3) ++HcMeleeDriven[W];
					HcMontageReadFrame = GFrameCounter + 2;	// 능력이 몽타주를 튼 뒤 — 같은 프레임엔 아직 안 돈다
					RequestEventShot(TEXT("melee"), 0.25, 3);
				}
			}
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

// ─────────────────────────────────────────────────────────────
// 같은 입력 누름 차례(L-5-c)

void UMOBridgeSubsystem::OnLyraInputTagPressed(const ULyraAbilitySystemComponent& Self, const FGameplayTag& InputTag)
{
	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn || FindASC(Pawn) != &Self) return;		// 남의 몸(상대 대조 · 기록 도구가 부르는 것)은 안 본다
	static FGameplayTag Tags[wire::kPressKeyCount];
	static bool bTagsReady = false;
	if (!bTagsReady)
	{
		for (int32 K = 0; K < wire::kPressKeyCount; ++K) Tags[K] = FGameplayTag::RequestGameplayTag(FName(kPressKeyTags[K]), false);
		bTagsReady = true;
	}
	for (uint8 K = 0; K < wire::kPressKeyCount; ++K)
	{
		if (Tags[K].IsValid() && InputTag == Tags[K])
		{
			PendingPressKeys.Add(K);
			++PressTags;
			return;
		}
	}
}

// 켜진 넷(근접 · 대시 · 수류탄 · 재장전)이 둘 이상이면 Lyra 가 부른 차례로 wire::MakePressOrder 값을 돌려준다(하나 이하면 0 — 서버가 안 본다).
// 차례 = 이 입력 몫(FramePressKeys) 다음 이번 틱 몫(PendingPressKeys)에서 켜진 단추의 첫 부름 차례. Lyra 는 한 프레임 안 누름을 Enhanced Input 묶음 차례로 부른다
// (EnhancedPlayerInput.cpp:739 — 누른 물리 차례가 아니다, 원본 기준). 켜졌는데 부름에 없는 단추(앞 프레임부터 누르고 있어 다시 안 부른 키)는 그 앞 프레임에 눌린 것이라 맨 앞에 둔다.
// OutText = 기록 줄 몫(둘 이상일 때만)
uint8 UMOBridgeSubsystem::MakeLyraPressOrder(uint8 Buttons, FString& OutText)
{
	int32 Count = 0;
	for (uint8 K = 0; K < wire::kPressKeyCount; ++K)
	{
		if ((Buttons & wire::kPressKeyButtons[K]) != 0) ++Count;
	}
	if (Count < 2) return 0;
	++PressMulti;
	uint8 Listed[wire::kPressKeyCount];
	int32 NumListed = 0;
	uint8 Seen = 0;
	for (const TArray<uint8>* List : { &FramePressKeys, &PendingPressKeys })
	{
		for (const uint8 K : *List)
		{
			if ((Buttons & wire::kPressKeyButtons[K]) == 0 || (Seen & (1u << K)) != 0) continue;
			Seen = (uint8)(Seen | (1u << K));
			Listed[NumListed++] = K;
		}
	}
	uint8 Keys[wire::kPressKeyCount];
	int32 N = 0;
	for (uint8 K = 0; K < wire::kPressKeyCount; ++K)
	{
		if ((Buttons & wire::kPressKeyButtons[K]) != 0 && (Seen & (1u << K)) == 0) Keys[N++] = K;
	}
	const int32 Unlisted = N;
	if (Unlisted > 0) ++PressUnlisted;
	for (int32 I = 0; I < NumListed; ++I) Keys[N++] = Listed[I];
	if (BreakPressOrder == 2)
	{
		for (int32 I = 0; I < N / 2; ++I) Swap(Keys[I], Keys[N - 1 - I]);
	}
	FString Order;
	for (int32 I = 0; I < N; ++I)
	{
		if (I > 0) Order.AppendChar(TEXT('>'));
		Order.AppendChar(kPressKeyLetters[Keys[I]]);
	}
	OutText = FString::Printf(TEXT("order=%s unlisted=%d tags=%d+%d"), *Order, Unlisted, FramePressKeys.Num(), PendingPressKeys.Num());
	if (BreakPressOrder == 1) return 0;
	++PressSent;
	return wire::MakePressOrder(Keys, N);
}

// 내 몸 입력 칸의 Triggered 묶음 중 넷(근접 · 대시 · 수류탄 · 재장전)의 차례 — Lyra 가 같은 프레임 누름을 부르는 차례(키 시험 판정의 정답지)
FString UMOBridgeSubsystem::ReadPressBindings() const
{
	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	const UEnhancedInputComponent* EIC = Pawn ? Cast<UEnhancedInputComponent>(Pawn->InputComponent) : nullptr;
	if (!EIC) return TEXT("none");
	FString Order;
	uint8 Seen = 0;
	for (const TUniquePtr<FEnhancedInputActionEventBinding>& Binding : EIC->GetActionEventBindings())
	{
		if (!Binding || Binding->GetTriggerEvent() != ETriggerEvent::Triggered || !Binding->GetAction()) continue;
		const FString Name = Binding->GetAction()->GetName();
		for (uint8 K = 0; K < wire::kPressKeyCount; ++K)
		{
			if (Name != kPressKeyActions[K] || (Seen & (1u << K)) != 0) continue;
			Seen = (uint8)(Seen | (1u << K));
			Order.AppendChar(kPressKeyLetters[K]);
		}
	}
	return Order.IsEmpty() ? FString(TEXT("empty")) : Order;
}

// 키 시험(-MOBPressKeyTest) — 사람 키 길(키 사건 → Enhanced Input → Lyra 영웅 입력 → AbilityInputTagPressed 갈고리 → 이 다리)을 기계로 탄다.
// 칸마다 3초(8초부터): 첫 키 누름 · 둘째 키는 같은 프레임(둘이 넣는 차례만 바꾼 짝 — 묶음 차례가 이기면 같은 차례가 나와야 한다) 또는 첫 키를 누른 채 3프레임 뒤 ·
// 둘째 키 6프레임 뒤 둘 다 뗌. 키 = 다리가 읽는 사람 키(C 근접 · 왼 Shift 대시 · Q 수류탄 · R 재장전)
void UMOBridgeSubsystem::TickPressKeyTest(double T)
{
	struct FCell
	{
		const TCHAR* Name;
		const FKey* First;
		const FKey* Second;
		int32 Delay;
	};
	static const FCell Cells[] = {
		{ TEXT("melee_dash"), &EKeys::C, &EKeys::LeftShift, 0 },
		{ TEXT("dash_melee"), &EKeys::LeftShift, &EKeys::C, 0 },
		{ TEXT("melee_q"), &EKeys::C, &EKeys::Q, 0 },
		{ TEXT("q_melee"), &EKeys::Q, &EKeys::C, 0 },
		{ TEXT("reload_q"), &EKeys::R, &EKeys::Q, 0 },
		{ TEXT("q_reload"), &EKeys::Q, &EKeys::R, 0 },
		{ TEXT("melee_hold_dash"), &EKeys::C, &EKeys::LeftShift, 3 },
	};
	constexpr double StartAt = 8.0, CellSeconds = 3.0;
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (!PC || !bHasSpawn || T < StartAt || bPressKeyDone) return;
	const int32 Want = (int32)((T - StartAt) / CellSeconds);
	if (Want != PressKeyCell)
	{
		if (PressKeyCell >= 0 && PressKeyPhase < 3) UE_LOG(LogMOB, Warning, TEXT("MOB_PRESSKEY_CUT cell=%d phase=%d"), PressKeyCell, PressKeyPhase);
		if (Want >= (int32)UE_ARRAY_COUNT(Cells))
		{
			bPressKeyDone = true;
			UE_LOG(LogMOB, Log, TEXT("MOB_PRESSKEY_DONE cells=%d"), (int32)UE_ARRAY_COUNT(Cells));
			return;
		}
		PressKeyCell = Want;
		PressKeyPhase = 0;
		PressKeyFrame = GFrameCounter;
	}
	const FCell& C = Cells[PressKeyCell];
	// 장치 = 내 사용자의 주 장치 — Lyra 는 bFilterInputByPlatformUser=True(DefaultInput.ini)라 장치 없음(INPUTDEVICEID_NONE)으로 넣으면 엔진이 남의 입력으로 보고 버린다(PlayerController.cpp:2412, 첫 판 l5c_keys 누름 표식 0)
	const FInputDeviceId Device = IPlatformInputDeviceMapper::Get().GetPrimaryInputDeviceForUser(PC->GetPlatformUserId());
	auto Send = [PC, Device](const FKey& Key, EInputEvent Event)
	{
		PC->InputKey(FInputKeyEventArgs::CreateSimulated(Key, Event, Event == IE_Pressed ? 1.f : 0.f, -1, Device));
	};
	const uint64 Age = GFrameCounter - PressKeyFrame;
	if (PressKeyPhase == 0)
	{
		Send(*C.First, IE_Pressed);
		if (C.Delay == 0) Send(*C.Second, IE_Pressed);
		UE_LOG(LogMOB, Log, TEXT("MOB_PRESSKEY cell=%d name=%s frame=%llu delay=%d"), PressKeyCell, C.Name, (unsigned long long)GFrameCounter, C.Delay);
		PressKeyPhase = C.Delay == 0 ? 2 : 1;
	}
	else if (PressKeyPhase == 1 && Age >= (uint64)C.Delay)
	{
		Send(*C.Second, IE_Pressed);
		// 둘째 키를 넣은 프레임 — Lyra 는 다음 프레임에 부른다. 판정이 「그 입력이 그보다 앞 프레임부터 걸쳤나(누른 근접 부름이 먼저 드나)」를 이 값으로 가른다
		UE_LOG(LogMOB, Log, TEXT("MOB_PRESSKEY_SECOND cell=%d frame=%llu"), PressKeyCell, (unsigned long long)GFrameCounter);
		PressKeyPhase = 2;
	}
	else if (PressKeyPhase == 2 && Age >= (uint64)C.Delay + 6)
	{
		Send(*C.First, IE_Released);
		Send(*C.Second, IE_Released);
		PressKeyPhase = 3;
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
	// 대시(-MOBAutoDash) — 3.6초(한 방향 걷기 1.2초 구간 셋)마다 두 번 0.05초 누름: 걷는 방향이 바뀌는 프레임(누른 입력의 이동이 앞 입력 이동과 다르다 — 다리가 마지막 이동 입력을
	// 바꿔 넣는 갈래) · 1.8초 뒤 걷기 구간의 한가운데. 두 누름 간격 1.8초 > 쿨다운 1.5초라 둘 다 켜진다(주기 · 자리는 제 판단, 3 과 8 이 서로소라 여덟 걷기 방향을 다 돈다).
	// 사람 키처럼 Lyra 입력 표식에도 누름을 넣는다 — Lyra 는 다음 프레임 입력 처리에서 보므로 다리도 다음 프레임에 읽는다(bAutoDash = 한 프레임 앞 키)
	const double DashPhase = FMath::Fmod(S, 3.0 * kAutoLeg);
	const bool bDashKeyNow = bAutoDashOn && (DashPhase < 0.05 || (DashPhase >= 1.5 * kAutoLeg && DashPhase < 1.5 * kAutoLeg + 0.05));
	{
		static const FGameplayTag DashTag = FGameplayTag::RequestGameplayTag(TEXT("InputTag.Ability.Dash"), false);
		ULyraAbilitySystemComponent* LASC = Cast<ULyraAbilitySystemComponent>(FindASC(Ch));
		if (LASC && DashTag.IsValid())
		{
			if (bDashKeyNow && !bAutoDashKey) LASC->AbilityInputTagPressed(DashTag);
			else if (!bDashKeyNow && bAutoDashKey) LASC->AbilityInputTagReleased(DashTag);
		}
	}
	bAutoDash = bAutoDashKey;
	bAutoDashKey = bDashKeyNow;
	// 수류탄(-MOBAutoGrenade) — 2.6초마다 0.05초 누름(쿨다운 5초 · 301입력이라 둘에 하나는 쿨다운에 걸려 서버 · 예측이 버린다 — 버림 길도 지나게) ·
	// 20초마다 재장전 누름(아래 bAutoReload, 19초부터) 0.4초 뒤 한 번(재장전 중 던짐 — 끊기 길). 주기 · 자리는 제 판단.
	// 대시와 같이 Lyra 입력 표식에도 누름을 넣고(뗐으면 아무 일도 없어야 한다 — -MOBBreakGrenadeBlock 이면 Lyra 가 혼자 켠다) 다리는 다음 프레임에 읽는다
	const double ReloadCycle = FMath::Fmod(S, 20.0);
	const bool bGrenadeKeyNow = bAutoGrenadeOn && (FMath::Fmod(S, 2.6) < 0.05 || (ReloadCycle >= 19.4 && ReloadCycle < 19.45));
	{
		static const FGameplayTag GrenadeTag = FGameplayTag::RequestGameplayTag(TEXT("InputTag.Weapon.Grenade"), false);
		ULyraAbilitySystemComponent* LASC = Cast<ULyraAbilitySystemComponent>(FindASC(Ch));
		if (LASC && GrenadeTag.IsValid())
		{
			if (bGrenadeKeyNow && !bAutoGrenadeKey) LASC->AbilityInputTagPressed(GrenadeTag);
			else if (!bGrenadeKeyNow && bAutoGrenadeKey) LASC->AbilityInputTagReleased(GrenadeTag);
		}
	}
	bAutoGrenade = bAutoGrenadeKey;
	bAutoGrenadeKey = bGrenadeKeyNow;
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
	// 무기 — 10초마다 다음 빠른 칸(W-4, 키 1 · 2 · 3 과 같은 길 — 빈 칸이면 안 바꾼다). 세 무기로 시작한 판(--testStartAllWeapons)은 칸 i = 무기 i 라 예전 무기 번호 순환과 같다.
	// -MOBAutoWeapon= 은 무기 번호 고정 그대로
	if (AutoWeaponFixed >= 0)
	{
		AutoWeapon = (uint8)AutoWeaponFixed;
		AutoSlot = -1;
	}
	else
	{
		AutoSlot = (int32)(S / 10.0) % (int32)mo::kQuickSlotCount;
	}
}

// ─────────────────────────────────────────────────────────────
//  무기 각본(-MOBWeaponScript, W-4) — 서버 프로브 RunWeapons(Tools/ServerProbe/main.cpp)와 같은 줄 · 같은 길이를
//  프로브가 안 쓰는 받침 넷에서 돈다(프로브 R1 · R3 · P7 · S5 ↔ 여기 R2 · R4 · P8 · S6). 두 사람이 같은 판에서 같이 돌아도 받침이 안 겹친다.
//  자리 · 탄 · 죽음 = 시험 입력(C2S_DEBUG_PLACE — TickInput 끝에서 보냄), 칸 고르기 · 재장전 = 보통 입력(키 1 · 2 · 3 · R 과 같은 길).
//  움직임 · 사격 · 조준 · 근접 · 앉기 · 점프 없음, 시선 고정(요 0)
// ─────────────────────────────────────────────────────────────
namespace
{
	enum class EWsAct : uint8 { Home, Pad, Select, SetAmmo, Reload, Kill, None };
	struct FWsStep { const TCHAR* Name; double Seconds; EWsAct Act; int32 Arg; };
	// Select 의 Arg = 빠른 칸(프로브는 무기 번호 — 그때 가방이 [권총, 소총, 샷건] 이라 같은 무기를 든다)
	const FWsStep kWsSteps[] = {
		{ TEXT("settle"), 1.0, EWsAct::Home, 0 },
		{ TEXT("rifle_r2"), 14.0, EWsAct::Pad, 0 },
		{ TEXT("away1"), 1.0, EWsAct::Home, 0 },
		{ TEXT("slot0"), 1.5, EWsAct::Select, 0 },
		{ TEXT("rifle_low"), 0.5, EWsAct::SetAmmo, 0 },
		{ TEXT("rifle_r4"), 2.0, EWsAct::Pad, 1 },
		{ TEXT("away2"), 1.0, EWsAct::Home, 0 },
		{ TEXT("pistol_p8"), 2.0, EWsAct::Pad, 2 },
		{ TEXT("away3"), 1.0, EWsAct::Home, 0 },
		{ TEXT("pistol_low"), 0.5, EWsAct::SetAmmo, 1 },
		{ TEXT("reload_press"), 0.1, EWsAct::Reload, 0 },
		{ TEXT("shotgun_s6"), 3.0, EWsAct::Pad, 3 },
		{ TEXT("away4"), 1.0, EWsAct::Home, 0 },
		{ TEXT("pistol_low2"), 0.5, EWsAct::SetAmmo, 1 },
		{ TEXT("reload_pistol"), 3.0, EWsAct::Reload, 0 },
		{ TEXT("slot1"), 1.5, EWsAct::Select, 1 },
		{ TEXT("rifle_low2"), 0.5, EWsAct::SetAmmo, 2 },
		{ TEXT("reload_rifle"), 3.0, EWsAct::Reload, 0 },
		{ TEXT("slot2"), 1.5, EWsAct::Select, 2 },
		{ TEXT("shotgun_low"), 0.5, EWsAct::SetAmmo, 3 },
		{ TEXT("reload_shotgun"), 4.0, EWsAct::Reload, 0 },
		{ TEXT("die"), 15.0, EWsAct::Kill, 0 },
		{ TEXT("end"), 1.0, EWsAct::None, 0 },
	};
	constexpr int32 kWsStepCount = UE_ARRAY_COUNT(kWsSteps);
	// 집 = 프로브 집(4700, -250, -368.1)의 맞은편. 받침 위 선 자리 = 프로브 받침 자리의 맞은편(맵이 원점 대칭) — 프로브 값이 Lyra 기록(cprobe_w0a)의 선 자리다.
	// 받침 번호는 서버 받침 소식(받침 파일 값)에서 종류가 같고 이 자리에서 60 안인 것으로 찾는다 — 못 찾으면 그 칸을 건너뛰고 센다(WsNoPad)
	const FVector kWsHome(-4700.0, 250.0, -368.1);
	struct FWsSpot { mo::EPadKind Kind; FVector Stand; };
	const FWsSpot kWsSpots[4] = {
		{ mo::EPadKind::Rifle, FVector(12.1, 1634.6, 95.9) },			// 0 = R2 소총(B_WeaponSpawner2) — 새 무기, 든다
		{ mo::EPadKind::Rifle, FVector(0.0, -1623.7, 95.9) },			// 1 = R4 소총(B_WeaponSpawner4) — 가진 무기, 예비 채움
		{ mo::EPadKind::Pistol, FVector(4586.5, -1008.4, -368.031) },	// 2 = P8 권총(B_WeaponSpawner8) — 예비 가득, 실패(판 14 부터 실패 소식 → 받침 그래프 실패 길 문구, L-9-a)
		{ mo::EPadKind::Shotgun, FVector(-1472.0, 0.0, -1205.475) },	// 3 = S6 샷건(B_WeaponSpawner6) — 재장전 중 새 무기, 안 든다
	};
	struct FWsAmmo { uint8 Weapon; int16 Magazine, Spare; };
	const FWsAmmo kWsAmmo[4] = { { 1, 10, 20 }, { 0, 4, 48 }, { 1, 10, 60 }, { 2, 2, 16 } };	// 프로브 ammoSets 와 같은 차례
}

void UMOBridgeSubsystem::TickWeaponScript(double T)
{
	bAutoFire = false;
	bAutoAds = false;
	bAutoMelee = false;
	bAutoCrouch = false;
	bAutoJump = false;
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	ACharacter* Ch = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
	if (!Ch || !Core || !bHasSpawn || T < kAutoStart || (WsStep < 0 && MatchPhase != (uint8)wire::EMatchPhase::Playing))
	{
		AutoSlot = -1;
		AutoWeapon = Weapon;
		return;
	}
	PC->SetControlRotation(FRotator::ZeroRotator);

	if (WsStep < 0)
	{
		// 받침 찾기 — 판 시작 현황(PICKUP_SYNC)이 와 있다
		for (int32 i = 0; i < 4; ++i)
		{
			WsPadIndex[i] = INDEX_NONE;
			for (int32 p = 0; p < Pads.Num(); ++p)
			{
				if (Pads[p].bKnown && Pads[p].Kind == (uint8)kWsSpots[i].Kind && FVector::Dist2D(Pads[p].Location, kWsSpots[i].Stand) < 60.0) WsPadIndex[i] = p;
			}
			if (WsPadIndex[i] == INDEX_NONE) ++WsNoPad;
		}
		UE_LOG(LogMOB, Log, TEXT("MOB_WS_PADS r2=%d r4=%d p8=%d s6=%d noPad=%lld"), WsPadIndex[0], WsPadIndex[1], WsPadIndex[2], WsPadIndex[3], WsNoPad);
	}
	const bool bAdvance = WsStep < 0 || (WsStep < kWsStepCount && T - WsStepStart >= kWsSteps[WsStep].Seconds);
	if (bAdvance)
	{
		++WsStep;
		WsStepStart = T;
		if (WsStep < kWsStepCount)
		{
			const FWsStep& St = kWsSteps[WsStep];
			UE_LOG(LogMOB, Log, TEXT("MOB_WS_STEP step=%d name=%s t=%.2f seq=%u"), WsStep, St.Name, T, Seq);
			WsPlaceFlags = 0;
			WsAmmoWeapon = 0xFF;
			WsAmmoMag = 0;
			WsAmmoSpare = 0;
			switch (St.Act)
			{
			case EWsAct::Home:
				WsPlaceAt = kWsHome;
				bWsPlacePending = true;
				break;
			case EWsAct::Pad:
				if (WsPadIndex[St.Arg] == INDEX_NONE) break;
				WsPlaceAt = kWsSpots[St.Arg].Stand;
				bWsPlacePending = true;
				break;
			case EWsAct::SetAmmo:
				WsPlaceAt = kWsHome;
				WsPlaceFlags = wire::DEBUG_PLACE_SET_AMMO;
				WsAmmoWeapon = kWsAmmo[St.Arg].Weapon;
				WsAmmoMag = kWsAmmo[St.Arg].Magazine;
				WsAmmoSpare = kWsAmmo[St.Arg].Spare;
				bWsPlacePending = true;
				break;
			case EWsAct::Kill:
				WsPlaceAt = kWsHome;
				WsPlaceFlags = wire::DEBUG_PLACE_KILL;
				bWsPlacePending = true;
				break;
			case EWsAct::Select:
				WsSlot = St.Arg;
				break;
			case EWsAct::Reload:
				bWsReloadPending = true;
				WsReloadMark = InputsReload;
				break;
			case EWsAct::None:
				break;
			}
		}
	}
	// 칸 고르기 — 키처럼 그 칸 무기로 바뀔 때까지(TickInput 이 입력을 낸 프레임에 바꾼다). 빈 칸이면 안 바뀜(키와 같다)
	if (WsSlot >= 0)
	{
		const uint8 SlotWeapon = Core->Prediction.GetLoadout().SlotWeapon((size_t)WsSlot);
		if (SlotWeapon == mo::kNoWeapon || Weapon == SlotWeapon) WsSlot = -1;
	}
	AutoSlot = WsSlot;
	AutoWeapon = Weapon;
	// 재장전 — 단추가 실린 입력이 나갈 때까지 누르고(이 프레임에 입력이 없을 수 있다) 그 뒤 뗀다. Lyra 입력 표식에도 같이(자동 입력과 같은 길)
	if (bWsReloadPending && InputsReload > WsReloadMark) bWsReloadPending = false;
	bAutoReload = bWsReloadPending;
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
}

// ─────────────────────────────────────────────────────────────
//  사람 확인 자동화(-MOBHumanCheck) — human-check-auto-plan.md
// ─────────────────────────────────────────────────────────────

// 각본 — 자동 입력과 같은 깃발(bAutoFire · bAutoAds · bAutoMelee · bAutoCrouch · bAutoJump · bAutoReload)과 같은 Lyra 입력 표식을 쓴다. 끝나면 자동 입력이 그대로 이어받는다
bool UMOBridgeSubsystem::TickHumanScript(double T)
{
	if (HcStep >= kHumanStepCount) return false;
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	ACharacter* Ch = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
	const double Dt = HcLastT < 0 ? 0.0 : T - HcLastT;
	HcLastT = T;
	if (!Ch || !bHasSpawn || T < kAutoStart) return true;

	ULyraAbilitySystemComponent* LASC = Cast<ULyraAbilitySystemComponent>(FindASC(Ch));
	// bEveryFrame = 누른 동안 매 프레임 넣는다(조준 · 근접 — 자동 입력과 같음). 아니면 바뀔 때만(사격 · 재장전)
	auto SetTag = [LASC](const TCHAR* Name, bool bNow, bool bWas, bool bEveryFrame)
	{
		const FGameplayTag InputTag = FGameplayTag::RequestGameplayTag(Name, false);
		if (!LASC || !InputTag.IsValid()) return;
		if (bNow && (bEveryFrame || !bWas)) LASC->AbilityInputTagPressed(InputTag);
		else if (!bNow && bWas) LASC->AbilityInputTagReleased(InputTag);
	};
	auto Apply = [&](bool bFire, bool bAds, bool bMelee, bool bReload)
	{
		SetTag(TEXT("InputTag.Weapon.Fire"), bFire, bAutoFire, false);
		SetTag(TEXT("InputTag.Weapon.ADS"), bAds, bAutoAds, true);
		SetTag(TEXT("InputTag.Ability.Melee"), bMelee, bAutoMelee, true);
		SetTag(TEXT("InputTag.Weapon.Reload"), bReload, bAutoReloadPressed, false);
		bAutoFire = bFire;
		bAutoAds = bAds;
		bAutoMelee = bMelee;
		bAutoReload = bReload;
		bAutoReloadPressed = bReload;
	};
	auto PressCrouch = [&]()
	{
		HcCrouchUntil = 0.1;	// 이 칸 시계 0.1초까지 BUTTON_CROUCH 를 누른 채(누름 순간은 TickInput 이 bCrouchWasDown 으로 가른다)
		if (ALyraCharacter* LyraCh = Cast<ALyraCharacter>(Ch)) LyraCh->ToggleCrouch();	// 사람 키와 같은 길(막기 갈고리가 되돌린다)
	};

	const ULyraHealthComponent* HC = ULyraHealthComponent::FindHealthComponent(Ch);
	const bool bDead = (ServerFlags & wire::STATE_DEAD) || (HC && HC->IsDeadOrDying());
	if (bDead)
	{
		if (!bHcDead && bHcStepBegun)
		{
			// 죽음 — 이 구역 첫 칸부터 다시(부활 자리가 바뀌므로 벽도 다시 고른다)
			int32 Back = HcStep;
			while (Back > 0 && !kHumanSteps[Back].bBlock) --Back;
			UE_LOG(LogMOB, Log, TEXT("MOB_HC_RESTART t=%.2f step=%s back=%s"), T, kHumanSteps[HcStep].Name, kHumanSteps[Back].Name);
			HcStep = Back;
			++HcRestarts;
		}
		bHcDead = true;
		bHcStepBegun = false;
		HcCJumpPressFrame = 0;
		if (bAutoJump) Ch->StopJumping();
		bAutoJump = false;
		bAutoCrouch = false;
		HcCrouchUntil = -1;
		Apply(false, false, false, false);
		return true;
	}
	bHcDead = false;

	const FHcStep& St = kHumanSteps[HcStep];
	if (!bHcStepBegun)
	{
		bHcStepBegun = true;
		HcStepClock = 0;
		HcStepShots = 0;
		HcNextShotAt = 0;
		HcFireUntil = -1;
		HcCrouchUntil = -1;
		HcNextPathAt = 0;
		HcPath.Reset();
		HcNavPaths = 0;
		HcApproachDist = -1;
		HcApproachHits0 = Core ? Core->Sim.GetCounters().pawnHits : 0;
		if (St.bBlock || HcWallDist < 0)
		{
			// 벽 고르기 — 눈높이에서 16 방향으로 30m 광선, 맞은 거리가 4~25m 인 것 중 10m 에 가장 가까운 방향(제 판단: 산탄 자국이 화면에 들어오는 거리).
			// 없으면 맞은 것 중 가장 가까운 방향
			const FVector Eye = Ch->GetPawnViewLocation();
			FCollisionQueryParams Q(SCENE_QUERY_STAT(MOBHumanWall), false, Ch);
			double BestScore = -1, BestDist = -1, Nearest = -1;
			float BestYaw = 0.f, NearestYaw = 0.f;
			for (int32 i = 0; i < 16; ++i)
			{
				const float Yaw = i * 22.5f;
				FHitResult Hit;
				if (!GetWorld()->LineTraceSingleByChannel(Hit, Eye, Eye + FRotator(0.f, Yaw, 0.f).Vector() * 3000.0, ECC_Visibility, Q)) continue;
				const double D = Hit.Distance;
				if (Nearest < 0 || D < Nearest) { Nearest = D; NearestYaw = Yaw; }
				if (D < 400.0 || D > 2500.0) continue;
				const double Score = 1.0 / (1.0 + FMath::Abs(D - 1000.0));
				if (Score > BestScore) { BestScore = Score; BestYaw = Yaw; BestDist = D; }
			}
			HcYaw = BestScore > 0 ? BestYaw : NearestYaw;
			HcWallDist = BestScore > 0 ? BestDist : Nearest;
		}
		HcHipYawOffset = 0.f;
		if (St.Act == EHcAct::HipFire)
		{
			// 허리 사격은 8° 옆을 겨눈다 — 조준 자국 무리와 안 겹치게. 알 각 최대 조준 2.33° + 허리 2.69° = 5.0°(파일럿 ha2 72알씩)에 3° 틈(10m 에서 약 50cm).
			// 4° 는 ha2 확대 사진에서 두 무리가 겹쳤다
			// 양쪽 중 벽 거리가 가운데와 가까운 쪽(같은 벽일 가능성이 큰 쪽)
			const FVector Eye = Ch->GetPawnViewLocation();
			FCollisionQueryParams Q(SCENE_QUERY_STAT(MOBHumanWall), false, Ch);
			double BestGap = -1;
			for (const float Side : { 8.f, -8.f })
			{
				FHitResult Hit;
				if (!GetWorld()->LineTraceSingleByChannel(Hit, Eye, Eye + FRotator(0.f, HcYaw + Side, 0.f).Vector() * 3000.0, ECC_Visibility, Q)) continue;
				const double Gap = FMath::Abs(Hit.Distance - HcWallDist);
				if (BestGap < 0 || Gap < BestGap) { BestGap = Gap; HcHipYawOffset = Side; }
			}
		}
		if (St.Act == EHcAct::CrouchPress && !Ch->bIsCrouched) PressCrouch();
		if (St.Act == EHcAct::StandUp && Ch->bIsCrouched) PressCrouch();
		if (St.Act == EHcAct::CrouchJump)
		{
			HcCJumpPressFrame = GFrameCounter;
			HcCJumpPressT = T;
			bHcCJumpCrouched = Ch->bIsCrouched;
			HcCJumpUncrouch = HcCJumpLift = -1;
		}
		UE_LOG(LogMOB, Log, TEXT("MOB_HC_STEP begin step=%s t=%.2f frame=%llu seq=%u yaw=%.1f wall=%.0f hipYaw=%.1f crouched=%d weapon=%u"), St.Name, T, (unsigned long long)GFrameCounter, Seq, HcYaw, HcWallDist,
			HcHipYawOffset, Ch->bIsCrouched ? 1 : 0, (uint32)Core->Prediction.GetWeaponIndex());
	}
	else
	{
		HcStepClock += Dt;
	}

	PC->SetControlRotation(FRotator(0.f, HcYaw + HcHipYawOffset, 0.f));	// 높낮이 0 — 기록 도구(MOBContentProbeSubsystem Prepare)와 같은 시선
	if (St.Act == EHcAct::MeleeHold)
	{
		// 근접 칸은 4m 안 가장 가까운 살아 있는 적을 바라본다 — 붙어 있는 적을 쳐 밀림 · 맞은 반응이 나게(파일럿 ha1 · ha2 는 근접이 적에 한 번도 안 닿았다. 4m 는 제 판단)
		double Nearest = 400.0;
		for (const TPair<uint32, FMOBRemote>& Pair : Remotes)
		{
			const FMOBRemote& R = Pair.Value;
			if (R.ServerTeam == Team || R.bKilled || !R.Pawn.IsValid()) continue;
			const FVector To = R.Pawn->GetActorLocation() - Ch->GetActorLocation();
			if (To.Size2D() >= Nearest) continue;
			Nearest = To.Size2D();
			PC->SetControlRotation(FRotator(0.f, (float)To.Rotation().Yaw, 0.f));
		}
	}
	bool bApproachDone = false;
	if (St.Act == EHcAct::Approach)
	{
		// 가장 가까운 살아 있는 적에게 걷는다 — 길찾기 경로를 0.5초마다 내 자리에서 다시 구해 다음 굽이 쪽으로(경로가 없으면 곧장), 3m 안이면 곧장 밀고 들어간다.
		// 이 칸에서 내 예측이 남 캡슐에 새로 닿으면(pawnHits 가 늘면) 끝. 적이 없으면(죽어 다시 나오는 중) 선다
		const FMOBRemote* Best = nullptr;
		double BestD = 0;
		for (const TPair<uint32, FMOBRemote>& Pair : Remotes)
		{
			const FMOBRemote& R = Pair.Value;
			if (R.ServerTeam == Team || R.bKilled || !R.Pawn.IsValid()) continue;
			const double D = FVector::Dist2D(R.Pawn->GetActorLocation(), Ch->GetActorLocation());
			if (!Best || D < BestD) { Best = &R; BestD = D; }
		}
		if (Best)
		{
			const FVector Me = Ch->GetActorLocation();
			const FVector Goal = Best->Pawn->GetActorLocation();
			HcApproachDist = BestD;
			if (Core && Core->Sim.GetCounters().pawnHits > HcApproachHits0)
			{
				bApproachDone = true;
			}
			else
			{
				if (BestD >= kHcApproachStraight && HcStepClock >= HcNextPathAt)
				{
					HcNextPathAt = HcStepClock + 0.5;
					HcPath.Reset();
					const UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(GetWorld(), Me, Goal, Ch);
					if (Path && Path->IsValid())
					{
						HcPath = Path->PathPoints;
						++HcNavPaths;
					}
				}
				FVector Next = Goal;
				if (BestD >= kHcApproachStraight) for (const FVector& Point : HcPath)
				{
					if (FVector::Dist2D(Point, Me) > 60.0) { Next = Point; break; }	// 첫 점은 내 발밑 — 60cm 넘게 떨어진 첫 굽이
				}
				const FVector Dir = (Next - Me).GetSafeNormal2D();
				Ch->AddMovementInput(FVector::ForwardVector, (float)Dir.X);
				Ch->AddMovementInput(FVector::RightVector, (float)Dir.Y);
				PC->SetControlRotation(FRotator(0.f, (float)(Goal - Me).Rotation().Yaw, 0.f));
			}
		}
	}
	AutoWeapon = St.Weapon;
	AutoSlot = -1;		// 각본은 무기 번호로 고른다(W-4 칸 고르기 아님)
	bAutoCrouch = HcStepClock < HcCrouchUntil;
	if (St.Act == EHcAct::CrouchWalk)
	{
		const FVector Side = FRotator(0.f, HcYaw + 90.f, 0.f).Vector();	// 벽과 나란히
		Ch->AddMovementInput(FVector::ForwardVector, (float)Side.X);
		Ch->AddMovementInput(FVector::RightVector, (float)Side.Y);
	}
	const bool bJumpNow = St.Act == EHcAct::CrouchJump && HcStepClock < 0.1;	// 0.1초 누름(자동 입력과 같은 길이)
	if (bJumpNow && !bAutoJump) Ch->Jump();
	if (!bJumpNow && bAutoJump) Ch->StopJumping();
	bAutoJump = bJumpNow;

	const bool bFireStep = St.Act == EHcAct::AdsFire || St.Act == EHcAct::HipFire;
	if (bFireStep && HcStepShots < 8 && HcStepClock >= HcNextShotAt)
	{
		HcFireUntil = HcStepClock + 0.1;
		HcNextShotAt = HcStepClock + kHcFireEvery;
		++HcStepShots;
		RequestEventShot(St.Act == EHcAct::AdsFire ? TEXT("adsshot") : TEXT("hipshot"), 0.2, 2);
	}
	const bool bAdsNow = St.Act == EHcAct::AdsHold || St.Act == EHcAct::AdsFire;
	const bool bMeleeNow = St.Act == EHcAct::MeleeHold && HcStepClock >= kHcMeleeFrom;
	const bool bReloadNow = St.Act == EHcAct::Reload && HcStepClock < 0.2;
	Apply(bFireStep && HcStepClock < HcFireUntil, bAdsNow, bMeleeNow, bReloadNow);

	// 칸 시계가 이 프레임에 그 시각을 넘었나 — 사진을 칸마다 한 번만
	auto Crossed = [&](double At) { return HcStepClock >= At && HcStepClock - Dt < At; };
	// 재는 칸 끝 0.1초 전에 한 장
	if (St.bSample && Crossed(St.Seconds - 0.1)) RequestEventShot(Ch->bIsCrouched ? TEXT("crouched") : TEXT("standing"), 0.0, 2);
	// 자국 — 연기가 걷힌 뒤 허리 시야에서 찍는다(쏜 직후는 연기가 가리고, 조준 중엔 화면이 확대돼 크기를 못 견준다 — 파일럿 ha1).
	// 조준 자국 = 재장전 1.5초(마지막 조준 발 3.4초 뒤) · 허리 자국 = 허리 사격 끝 0.4초 전(마지막 발 1.4초 뒤)
	if (St.Act == EHcAct::Reload && Crossed(1.5)) RequestEventShot(TEXT("adsmarks"), 0.0, 2);
	if (St.Act == EHcAct::HipFire && Crossed(St.Seconds - 0.4)) RequestEventShot(TEXT("hipmarks"), 0.0, 2);
	if (HcStepClock < St.Seconds && !bApproachDone) return true;

	if (St.Act == EHcAct::Approach)
	{
		UE_LOG(LogMOB, Log, TEXT("MOB_HC_APPROACH step=%s contact=%d dist=%.0f sec=%.2f navPaths=%d"), St.Name, bApproachDone ? 1 : 0, HcApproachDist, HcStepClock, HcNavPaths);
	}
	UE_LOG(LogMOB, Log, TEXT("MOB_HC_STEP end step=%s t=%.2f frame=%llu seq=%u shots=%d crouched=%d weapon=%u"), St.Name, T, (unsigned long long)GFrameCounter, Seq, HcStepShots,
		Ch->bIsCrouched ? 1 : 0, (uint32)Core->Prediction.GetWeaponIndex());
	++HcStep;
	bHcStepBegun = false;
	if (HcStep < kHumanStepCount) return true;
	Apply(false, false, false, false);
	UE_LOG(LogMOB, Log, TEXT("MOB_HC_SCRIPT_DONE t=%.2f restarts=%d"), T, HcRestarts);
	return false;
}

// 매 프레임 — 각본이 끝난 뒤에도 돈다(부딪힘 · 맞은 반응 사진 · 끝 점수판)
void UMOBridgeSubsystem::SampleHumanCheck(double T)
{
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	ACharacter* Ch = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
	if (!Ch || !bHasSpawn) return;
	// 받침 판(-MOBStandAt, 자동 입력 없음) — 발밑 받침이 화면에 들어오게 거의 바로 위에서 내려다본다(파일럿 ha1 은 받침이 내 몸에 가려 대기 표시가 안 보였고,
	// −55°(ha5)도 받침이 몸 바로 밑이라 가렸다 — 바로 위면 몸보다 넓은 받침판 둘레가 드러난다. −85° 는 제 판단)
	if (bStandOnly) PC->SetControlRotation(FRotator(-85.f, PC->GetControlRotation().Yaw, 0.f));
	const UCharacterMovementComponent* M = Ch->GetCharacterMovement();
	if (Ch->bIsCrouched != bHcLastCrouched)
	{
		bHcLastCrouched = Ch->bIsCrouched;
		HcCrouchChangedAt = T;
	}

	// 카메라 · 낮은 발 높이 − 바닥(캡슐 밑). 정답지 = 같은 값의 Lyra 기록(기록 도구 camZ · footZ 열)과 식(앉으면 카메라 −48, 조각 1)
	if (HcStep < kHumanStepCount && bHcStepBegun && !bHcDead && kHumanSteps[HcStep].bSample && M && M->IsMovingOnGround() && T - HcCrouchChangedAt >= kHcSettle && PC->PlayerCameraManager)
	{
		const double Floor = Ch->GetActorLocation().Z - Ch->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		const int32 K = Ch->bIsCrouched ? 1 : 0;
		HcCam[K].Add(PC->PlayerCameraManager->GetCameraLocation().Z - Floor);
		const USkeletalMeshComponent* Mesh = Ch->GetMesh();
		double Foot = 0;
		bool bFoot = false;
		for (const TCHAR* Bone : { TEXT("ball_l"), TEXT("ball_r") })
		{
			if (!Mesh || Mesh->GetBoneIndex(Bone) == INDEX_NONE) continue;
			const double Z = Mesh->GetBoneLocation(Bone).Z;
			Foot = bFoot ? FMath::Min(Foot, Z) : Z;
			bFoot = true;
		}
		if (bFoot) HcFoot[K].Add(Foot - Floor);
		else ++HcFootMissing;
	}

	// 앉은 채 점프 — 누른 뒤 몇 프레임에 일어서고 몇 프레임에 떠오르나(서버 · 예측은 같은 입력에서 일어서기 → 점프, D-5)
	if (HcCJumpPressFrame)
	{
		const int32 Since = (int32)(GFrameCounter - HcCJumpPressFrame);
		if (HcCJumpUncrouch < 0 && !Ch->bIsCrouched) HcCJumpUncrouch = Since;
		if (HcCJumpLift < 0 && M && M->Velocity.Z > 1.0) HcCJumpLift = Since;
		if ((HcCJumpUncrouch >= 0 && HcCJumpLift >= 0) || Since > 60)
		{
			UE_LOG(LogMOB, Log, TEXT("MOB_HC_CJUMP crouchedAtPress=%d uncrouchFrames=%d liftFrames=%d sec=%.3f"), bHcCJumpCrouched ? 1 : 0, HcCJumpUncrouch, HcCJumpLift, T - HcCJumpPressT);
			RequestEventShot(TEXT("cjump"), 0.2, 2);
			HcCJumpPressFrame = 0;
			++HcCJumps;
		}
	}

	// 내 근접 능력을 켠 두 프레임 뒤 내 메시에서 도는 몽타주
	if (HcMontageReadFrame && GFrameCounter >= HcMontageReadFrame)
	{
		HcMontageReadFrame = 0;
		const UAnimInstance* AI = Ch->GetMesh() ? Ch->GetMesh()->GetAnimInstance() : nullptr;
		const UAnimMontage* Montage = AI ? AI->GetCurrentActiveMontage() : nullptr;
		++HcMyMontages.FindOrAdd(Montage ? Montage->GetName() : FString(TEXT("none")));
	}

	// 남 메시에서 도는 몽타주 — 바뀌어 새로 돌기 시작하면 이름별로 센다. 이름에 Hit 이 든 것(맞은 반응)이 내 8m 안 · 화면 가운데 30° 안이고
	// 내가 살아 있으면 사진(제 판단 — 파일럿 ha1 은 조건이 거리뿐이라 내가 맞는 중 · 죽은 뒤 · 화면 밖 반응을 찍었다)
	const ULyraHealthComponent* MyHC = ULyraHealthComponent::FindHealthComponent(Ch);
	const bool bAlive = !(ServerFlags & wire::STATE_DEAD) && !(MyHC && MyHC->IsDeadOrDying());
	const FVector CamLoc = PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : Ch->GetActorLocation();
	const FVector CamDir = PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraRotation().Vector() : Ch->GetActorForwardVector();
	for (const TPair<uint32, FMOBRemote>& Pair : Remotes)
	{
		const ACharacter* Other = Pair.Value.Pawn.Get();
		const UAnimInstance* OtherAnim = Other && Other->GetMesh() ? Other->GetMesh()->GetAnimInstance() : nullptr;
		const UAnimMontage* OtherMontage = OtherAnim ? OtherAnim->GetCurrentActiveMontage() : nullptr;
		const FString Name = OtherMontage ? OtherMontage->GetName() : FString();
		const float Pos = OtherMontage ? OtherAnim->Montage_GetPosition(OtherMontage) : 0.f;
		FString& Last = HcRemoteMontageNow.FindOrAdd(Pair.Key);
		float& LastPos = HcRemoteMontagePos.FindOrAdd(Pair.Key);
		if (!Name.IsEmpty() && (Name != Last || Pos < LastPos))
		{
			++HcRemoteMontageStarts.FindOrAdd(Name);
			const bool bInView = FVector::DotProduct(CamDir, (Other->GetActorLocation() - CamLoc).GetSafeNormal()) > 0.866;	// cos 30°
			const bool bNear = FVector::Dist(Other->GetActorLocation(), Ch->GetActorLocation()) < 800.0;
			if (Name.Contains(TEXT("Hit")) && bAlive && bInView && bNear) RequestEventShot(TEXT("react"), 0.05, 3);
			// 내 근접이 맞힌 그 상대가 1초 안에 맞은 반응을 시작했나(맞힘 하나에 한 번)
			if (Name.Contains(TEXT("Hit")) && Pair.Key == HcMeleeTarget && LastMyMeleeHitAt > 0 && FPlatformTime::Seconds() - LastMyMeleeHitAt < 1.0)
			{
				++HcMeleeReacts;
				HcMeleeTarget = 0;
				if (bAlive && bInView && bNear) RequestEventShot(TEXT("meleereact"), 0.05, 3);
			}
		}
		Last = Name;
		LastPos = Pos;
	}

	// 부딪힘 — 예측이 남 캡슐에 새로 닿으면 0.2초 뒤 사진(세 장까지)
	if (Core)
	{
		const uint64 Hits = Core->Sim.GetCounters().pawnHits;
		if (Hits > HcLastPawnHits) RequestEventShot(TEXT("bump"), 0.2, 3);
		HcLastPawnHits = Hits;
	}

	// 점수판 — 경기 결과가 오면(도움 대조 MOB_ASSIST_RESULT 뒤) 또는 -MOBQuitAt 3초 전에 Lyra 점수판 능력 입력(InputTag.Ability.ShowLeaderboard,
	// IA_ShowScoreboard = Tab)을 누른 채 두고 1.2초 뒤 찍는다. 줄마다 Lyra 점수판이 읽는 값(ShooterGame.Score.*)을 같이 남긴다 — 사진의 숫자와 견줄 것
	if (!bHcBoardPressed && (ResultAt > 0 || (QuitAt > 0 && T > QuitAt - 3.0)))
	{
		bHcBoardPressed = true;
		const FGameplayTag BoardTag = FGameplayTag::RequestGameplayTag(TEXT("InputTag.Ability.ShowLeaderboard"), false);
		ULyraAbilitySystemComponent* LASC = Cast<ULyraAbilitySystemComponent>(FindASC(Ch));
		if (LASC && BoardTag.IsValid()) LASC->AbilityInputTagPressed(BoardTag);
		UE_LOG(LogMOB, Log, TEXT("MOB_HC_BOARD t=%.2f tag=%d asc=%d afterResult=%d"), T, BoardTag.IsValid() ? 1 : 0, LASC ? 1 : 0, ResultAt > 0 ? 1 : 0);
		RequestEventShot(TEXT("board"), 1.2, 1);
		static const FGameplayTag ElimTag = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.Score.Eliminations"), false);
		static const FGameplayTag DeathTag = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.Score.Deaths"), false);
		static const FGameplayTag AssistTag = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.Score.Assists"), false);
		// id = 서버 번호(서버 SCORE id= 와 잇는다). 나 + 지금 아는 남 전부
		TArray<uint32> Ids;
		Remotes.GetKeys(Ids);
		Ids.Insert(PlayerId, 0);
		for (const uint32 Id : Ids)
		{
			const ALyraPlayerState* LPS = Cast<ALyraPlayerState>(FindPlayerState(Id));
			if (!LPS) { UE_LOG(LogMOB, Log, TEXT("MOB_HC_BOARDROW id=%u noState=1"), Id); continue; }
			UE_LOG(LogMOB, Log, TEXT("MOB_HC_BOARDROW id=%u name=%s team=%d elims=%d deaths=%d assists=%d"), Id, *LPS->GetPlayerName(), LPS->GetTeamId(),
				LPS->GetStatTagStackCount(ElimTag), LPS->GetStatTagStackCount(DeathTag), LPS->GetStatTagStackCount(AssistTag));
		}
	}
}

void UMOBridgeSubsystem::LogHumanCheck()
{
	auto Range = [](const FHcRange& R) { return FString::Printf(TEXT("%lld,%.2f,%.2f,%.2f"), R.N, R.N ? R.Sum / R.N : 0.0, R.Min, R.Max); };
	const double CamDiff = HcCam[0].N && HcCam[1].N ? HcCam[1].Sum / HcCam[1].N - HcCam[0].Sum / HcCam[0].N : 0.0;
	// cam · foot = 프레임 수,평균,최소,최대(바닥 위 cm). camDiff = 앉은 평균 − 선 평균
	UE_LOG(LogMOB, Log, TEXT("MOB_HC_SUMMARY steps=%d/%d restarts=%d camStand=%s camCrouch=%s camDiff=%.2f footStand=%s footCrouch=%s footMissing=%lld cjumps=%d hitReactTicks=%lld padPickupFx=%lld padRespawnFx=%lld board=%d"),
		HcStep, kHumanStepCount, HcRestarts, *Range(HcCam[0]), *Range(HcCam[1]), CamDiff, *Range(HcFoot[0]), *Range(HcFoot[1]), HcFootMissing, HcCJumps, AnimHitReactTicks,
		PadPickupEffects, PadRespawnEffects, bHcBoardPressed ? 1 : 0);
	static const TCHAR* WeaponNames[] = { TEXT("pistol"), TEXT("rifle"), TEXT("shotgun") };
	for (int32 W = 0; W < 3; ++W)
	{
		FString Gaps;
		for (int32 i = 1; i < HcMeleeSeqs[W].Num(); ++i) Gaps += FString::Printf(TEXT("%u "), HcMeleeSeqs[W][i] - HcMeleeSeqs[W][i - 1]);
		UE_LOG(LogMOB, Log, TEXT("MOB_HC_MELEE weapon=%s starts=%d driven=%lld gaps=[%s]"), WeaponNames[W], HcMeleeSeqs[W].Num(), HcMeleeDriven[W], *Gaps.TrimEnd());
	}
	for (const TPair<FString, int32>& Pair : HcMyMontages) UE_LOG(LogMOB, Log, TEXT("MOB_HC_MYMONTAGE name=%s count=%d"), *Pair.Key, Pair.Value);
	for (const TPair<FString, int32>& Pair : HcRemoteMontageStarts) UE_LOG(LogMOB, Log, TEXT("MOB_HC_REMOTEMONTAGE name=%s starts=%d"), *Pair.Key, Pair.Value);
	UE_LOG(LogMOB, Log, TEXT("MOB_HC_MELEEREACT hits=%d reacts=%d"), HcMeleeHits, HcMeleeReacts);
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
	UE_LOG(LogMOB, Log, TEXT("MOB_SUMMARY3 posNonZero=%lld velMax=%.3f corrections=%lld correctionAvg=%.2f correctionMax=%.2f visualSnaps=%lld pawnChanges=%lld selfDriftMax=%.3f selfDriftOver1cm=%lld selfDriftFromSetMove=%lld selfSetMoves=%lld selfSetMoveMax=%.3f modeSwitchRestores=%lld modeSwitchRestoreMax=%.3f maxFrameGap=%.3f"),
		PosDiffNonZero, VelDiffMax, Corrections, Corrections ? CorrectionSum / Corrections : 0.0, CorrectionMax, VisualSnaps, PawnChanges, SelfDriftMax, SelfDriftOver1cm, SelfDriftFromSetMove, SelfSetMoves, SelfSetMoveMax, GModeSwitchRestores, GModeSwitchRestoreMax, MaxFrameGap);
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
		// 대시(D-3) — 받은 내 소식 · 남의 소식 · 예측 쪽 고친 수(dashDiff = startMismatches + forceFixes + unconfirmed). 다리는 D-5 부터 Shift 로 보낸다
		const mo::CSelfPrediction::DashCounters& DC = Core->Prediction.GetDashCounters();
		UE_LOG(LogMOB, Log, TEXT("MOB_DASH mine=%lld others=%lld presses=%llu started=%llu respawnGained=%llu respawnLost=%llu results=%llu resultsNoSlot=%llu dashDiff=%llu startMismatches=%llu forceFixes=%llu unconfirmed=%llu replays=%llu replaysMissed=%llu meleeInReplay=%llu"),
			DashMine, DashOthers, (unsigned long long)DC.presses, (unsigned long long)DC.started, (unsigned long long)DC.respawnGained, (unsigned long long)DC.respawnLost, (unsigned long long)DC.results, (unsigned long long)DC.resultsNoSlot,
			(unsigned long long)(DC.startMismatches + DC.forceFixes + DC.unconfirmed), (unsigned long long)DC.startMismatches, (unsigned long long)DC.forceFixes, (unsigned long long)DC.unconfirmed,
			(unsigned long long)DC.replays, (unsigned long long)DC.replaysMissed, (unsigned long long)DC.meleeInReplay);
		// 대시 다리(D-5) — Lyra 능력(켤 차례 · 켬 · 실패 · 칸 없음 · 버림 · 기다린 프레임 · Lyra 혼자 · 뗀 칸) · 몽타주 대조 · 방향별 켬(앞 · 뒤 · 왼 · 오른) ·
		// 남의 효과 · 대시 중 내 자리 고침(전체 고침 수는 MOB_SUMMARY3 corrections)
		UE_LOG(LogMOB, Log, TEXT("MOB_DASH2 inputs=%lld queued=%lld driven=%lld activateFail=%lld noSpec=%lld dropped=%lld waitFrames=%lld lyraSelf=%lld stripped=%lld montChecked=%lld montMismatch=%lld montNone=%lld dirSwapMattered=%lld prevFromSlot=%lld dirs=%lld/%lld/%lld/%lld cues=%lld cueNoPawn=%lld cueDropped=%lld cuePending=%d cueWaitAvg=%.3f cueWaitMax=%.3f dashCorr=%lld dashCorrMax=%.2f"),
			InputsDash, DashQueued, DashDriven, DashActivateFail, DashNoSpec, DashDropped, DashWaitFrames, LyraDashSelf, LyraDashStripped, DashMontChecked, DashMontageMismatch, DashMontNone, DashDirSwapMattered, DashPrevFromSlot,
			DashDirCount[0], DashDirCount[1], DashDirCount[2], DashDirCount[3], DashCues, DashCueNoPawn, DashCueDropped, PendingDashCues.Num(),
			DashCues ? DashCueWaitSum / DashCues : 0.0, DashCueWaitMax, DashCorrections, DashCorrectionMax);
		// 수류탄(G-5-a) — 받은 소식(내 던짐 · 남의 던짐 · 터짐 · 모르는 종류 · 상태 패킷) · 예측 쪽 셈. grenadeDiff = 서버 소식으로 고친 던짐(어긋남 + 소식 없이 지나감 − 죽음을 늦게 안 몫).
		// 봇 식의 경기 끝 몫(unconfirmedPost)은 다리가 끝 단계에 단추를 안 실어 뺄 것이 없다. finalThrown = 칸마다 마지막으로 정한 던짐의 합(봇 판정과 같은 식) — 확인 안 된 몫(unacked)을 빼면 내 던짐 소식 수와 같아야 한다
		const mo::CSelfPrediction::GrenadeCounters& GC = Core->Prediction.GetGrenadeCounters();
		const uint64 GrenadeUnacked = Core->Prediction.CountGrenadeThrownUnacked();
		const uint64 GrenadeFinal = GC.thrown + GC.respawnThrown - GC.respawnLost + GC.replayGained - GC.replayLost;
		UE_LOG(LogMOB, Log, TEXT("MOB_GRENADE mine=%lld others=%lld booms=%lld badKind=%lld statePackets=%lld presses=%llu thrown=%llu respawn=+%llu/-%llu replay=+%llu/-%llu finalThrown=%llu unacked=%llu sameFinal=%d results=%llu resultsNoSlot=%llu grenadeDiff=%llu startMismatches=%llu unconfirmed=%llu unconfirmedDead=%llu replays=%llu replaysMissed=%llu meleeInReplay=%llu"),
			GrenadeMine, GrenadeOthers, GrenadeBooms, GrenadeBadKind, GrenadeStatePackets, (unsigned long long)GC.presses, (unsigned long long)GC.thrown, (unsigned long long)GC.respawnThrown, (unsigned long long)GC.respawnLost,
			(unsigned long long)GC.replayGained, (unsigned long long)GC.replayLost, (unsigned long long)GrenadeFinal, (unsigned long long)GrenadeUnacked, GrenadeFinal - GrenadeUnacked == (uint64)GrenadeMine ? 1 : 0,
			(unsigned long long)GC.results, (unsigned long long)GC.resultsNoSlot, (unsigned long long)(GC.startMismatches + GC.unconfirmed - GC.unconfirmedDead),
			(unsigned long long)GC.startMismatches, (unsigned long long)GC.unconfirmed, (unsigned long long)GC.unconfirmedDead, (unsigned long long)GC.replays, (unsigned long long)GC.replaysMissed, (unsigned long long)GC.meleeInReplay);
		// 수류탄 다리(G-5-a) — 단추를 실은 입력 · 그중 끝 단계 누름 · 예측이 던진 입력 · 다리가 냄 · 버림 · 몽타주 실패 · 몸 없음 · Lyra 혼자 · 뗀 칸 · 끊은 능력(그중 재장전) · 실은 camBack 범위
		UE_LOG(LogMOB, Log, TEXT("MOB_GRENADE2 inputs=%lld pressPost=%lld queued=%lld driven=%lld dropped=%lld montageFail=%lld noSpec=%lld lyraSelf=%lld stripped=%lld cancelled=%lld cancelledReload=%lld cancelLeft=%lld camBackMin=%.2f camBackMax=%.2f camBackBelow=%lld camBackAbove=%lld pending=%d"),
			InputsGrenade, GrenadePressPost, GrenadeQueued, GrenadeDriven, GrenadeDropped, GrenadeMontageFail, GrenadeNoSpec, LyraGrenadeSelf, LyraGrenadeStripped, GrenadeCancelled, GrenadeCancelledReload, GrenadeCancelLeft,
			InputsGrenade ? GrenadeCamBackMin : 0.0, InputsGrenade ? GrenadeCamBackMax : 0.0, GrenadeCamBackBelow, GrenadeCamBackAbove, bGrenadePending ? 1 : 0);
		// 겉모습 수류탄(G-5-b) — 끝날 때 남은 칸: 아직 안 만듦(화면 시점이 던진 틱에 못 닿음) · 날아가는 중 · 터짐 소식은 왔고 아직 안 냄 · 꼬리 기다림.
		// 판정 = 만듦 + 안 만듦 + 만들기 실패 = 번호 있는 던짐 · 낸 터짐 + 터짐 대기 = 터짐 소식(칸 있는 것) · 혼자 터짐 0 · 사라짐 0 · 지운 뒤 켜진 타이머 0
		int64 VisNotSpawned = 0, VisFlying = 0, VisBoomPending = 0, VisTrailWait = 0;
		for (const TPair<uint32, FMOBGrenadeVisual>& Pair : GrenadeVisuals)
		{
			const FMOBGrenadeVisual& G = Pair.Value;
			if (!G.bSpawned) ++VisNotSpawned;
			else if (G.SpawnFrame == 0) continue;
			else if (G.bBoomed) ++VisTrailWait;
			else if (G.bBoomKnown) ++VisBoomPending;
			else ++VisFlying;
		}
		UE_LOG(LogMOB, Log, TEXT("MOB_GRENADE3 throwsWithId=%lld throwsNoId=%lld dupId=%lld spawned=%lld spawnFail=%lld notSpawned=%lld noInstigator=%lld timersActive=%lld timersLeft=%lld noMovement=%lld booms=%lld boomUnknown=%lld visBooms=%lld boomPending=%lld flying=%lld trailWait=%lld stateEntries=%lld stateUnknown=%lld stateOld=%lld stateBounced=%lld stateBadSize=%lld bounceSounds=%lld hitSounds=%lld localDetonate=%lld gone=%lld lost=%lld underrun=%lld goneFrames=%lld~%lld goneSec=%.3f~%.3f showDelayN=%lld showDelay=%lld~%lld showDelayAvg=%.2f showNoPress=%lld"),
			GrenadeThrowsWithId, GrenadeThrowsNoId, GrenadeDupId, GrenadeVisSpawned, GrenadeVisSpawnFail, VisNotSpawned, GrenadeVisNoInstigator, GrenadeVisTimersActive, GrenadeVisTimersLeft, GrenadeVisNoMovement,
			GrenadeBooms, GrenadeBoomUnknown, GrenadeVisBooms, VisBoomPending, VisFlying, VisTrailWait, GrenadeStateEntries, GrenadeStateUnknown, GrenadeStateOld, GrenadeStateBounced, GrenadeStateBadSize,
			GrenadeVisBounceSounds, GrenadeVisHitSounds, LocalGrenadeDetonate, GrenadeVisGone, GrenadeVisLost, GrenadeVisUnderrun,
			GrenadeVisGone ? GrenadeGoneFramesMin : 0, GrenadeGoneFramesMax, GrenadeVisGone ? GrenadeGoneSecMin : 0.0, GrenadeGoneSecMax,
			GrenadeShowDelayN, GrenadeShowDelayN ? GrenadeShowDelayMin : 0, GrenadeShowDelayMax, GrenadeShowDelayN ? (double)GrenadeShowDelaySum / (double)GrenadeShowDelayN : 0.0, GrenadeShowNoPress);
		// 걸음 몸(L-8-b-2) — 받은 목록마다 넣은 산 수류탄 몸 · 다시 돌림 · 예측 걸음이 수류탄에 막힌 수(Sim 합 — 다시 돌림 포함)
		UE_LOG(LogMOB, Log, TEXT("MOB_GRENADEWALK lists=%lld bodies=%lld ownIgnored=%lld ended=%lld stale=%lld reruns=%lld rerunsNone=%lld standableHits=%llu"),
			GrenadeWalkLists, GrenadeWalkBodies, GrenadeWalkOwnIgnored, GrenadeWalkEnded, GrenadeWalkStale, GrenadeWalkReruns, GrenadeWalkRerunsNone, Core ? (unsigned long long)Core->Sim.GetCounters().standableHits : 0ull);
		// 터짐 신호 — 길에 들어간 수(다리 · 그 밖) · 표식 없음 · 신호 묶음에 그 표식 칸이 있나 · 그 칸의 신호 클래스가 올라와 있나(-1 = 신호 관리자 · 묶음을 못 찾음).
		// 판정 = 다리 길 = 낸 터짐(visBooms) · 그 밖 0 · 칸 1 · 올라옴 1. 클래스가 덜 올라온 채 온 신호는 엔진이 올린 뒤 다시 낸다(GameplayCueManager.cpp HandleMissingGameplayCue)
		int32 CueInSet = -1, CueLoaded = -1;
		if (UGameplayCueManager* GCM = UAbilitySystemGlobals::Get().GetGameplayCueManager())
		{
			if (UGameplayCueSet* Set = GCM->GetRuntimeCueSet())
			{
				const int32* Idx = Set->GameplayCueDataMap.Find(FGameplayTag::RequestGameplayTag(kGrenadeDetonateCueTag, false));
				CueInSet = Idx ? 1 : 0;
				if (Idx) CueLoaded = (Set->GameplayCueData.IsValidIndex(*Idx) && Set->GameplayCueData[*Idx].LoadedGameplayCueClass) ? 1 : 0;
			}
		}
		UE_LOG(LogMOB, Log, TEXT("MOB_GRENADE4 cueRoutedMine=%lld cueRoutedOther=%lld cueNoTag=%lld cueInSet=%d cueLoaded=%d routeHooked=%d"),
			GrenadeCueRoutedMine, GrenadeCueRoutedOther, GrenadeVisCueNoTag, CueInSet, CueLoaded, GrenadeCueRouteHandle.IsValid() ? 1 : 0);
	}
	// 같은 입력 누름 차례(L-5-c) — 둘 이상 누른 입력 · 차례를 실음 · 부름에 없던 단추가 있던 입력 · 받은 내 누름 표식 · 버린 표식 · 일부러 틀림 · Lyra 묶음 차례(키 넷의 Triggered 묶음)
	UE_LOG(LogMOB, Log, TEXT("MOB_PRESS multi=%lld sent=%lld unlisted=%lld tags=%lld tagsDropped=%lld break=%d keyTestCells=%d bind=%s"),
		PressMulti, PressSent, PressUnlisted, PressTags, PressTagsDropped, BreakPressOrder, bPressKeyTest ? PressKeyCell + 1 : 0, *ReadPressBindings());
	UE_LOG(LogMOB, Log, TEXT("MOB_SUMMARY4 remotes=%d remoteFail=%d frames=%lld underrun=%lld teleports=%lld driftMax=%.3f driftOver1cm=%lld yawDiffMax=%.3f lagAvg=%.1f behindTicksAvg=%.2f"),
		RemoteSpawned, RemoteSpawnFail, RemoteFrames, RemoteUnderrun, RemoteTeleports, RemoteDriftMax, RemoteDriftOver1cm, RemoteYawDiffMax, RemoteFrames ? RemoteLagSum / RemoteFrames : 0.0, RemoteFrames ? RemoteBehindSum / RemoteFrames : 0.0);

	if (bServerAnim)
	{
		UE_LOG(LogMOB, Log, TEXT("MOB_SERVERANIM_SUMMARY tailPlayers=%lld tailBad=%lld noState=%lld poseEvals=%lld poseFrames=%lld poseFail=%lld"), AnimTailPlayers, AnimTailBad, AnimNoState, AnimPoseEvals, AnimPoseFrames, AnimPoseFail);
		// 무기별(9단계) — 무기별 자세 계산 수의 합 = poseEvals. setChanges = 언리얼 경로 클립 한 벌을 갈아 넣은 수, setMisses = 그 무기 한 벌이 없던 프레임, badWeapon = 모르는 무기 번호로 받은 칸
		// adsLayers = 언리얼 경로에서 조준 전환 층을 허리 전환으로 바꿔 튼 수, badLayers = 칸 밖이라 버린 층(0 이어야 함, C-5)
		// crouchLayers = 앉은 전환 층을 허리 전환으로 바꿔 튼 수, stanceLayers = 서기 안 앉기 · 일어서기 전환 층을 걷기 · 서기 자리로 돌린 수(D-3)
		// idleSlotLayers = 서기 자리 층(-2)을 걷기 · 서기 자리로 돌린 수(판 17 · L-13-a — 서 있다 움직이는 상대가 있으면 0 보다 커야 한다)
		UE_LOG(LogMOB, Log, TEXT("MOB_SERVERANIM_WEAPON poseEvals pistol=%lld rifle=%lld shotgun=%lld setChanges=%lld setMisses=%lld badWeapon=%lld adsLayers=%lld badLayers=%lld crouchLayers=%lld stanceLayers=%lld idleSlotLayers=%lld"),
			AnimPoseEvalsByWeapon[0], AnimPoseEvalsByWeapon[1], AnimPoseEvalsByWeapon[2], AnimSetChanges, AnimSetMisses, AnimBadWeapon, AnimAdsLayers, AnimBadLayers, AnimCrouchLayers, AnimStanceLayers, AnimIdleSlotLayers);
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
	{
		int32 Teleports = 0;
		for (const FMOBDevice& D : Devices) Teleports += D.bTeleport ? 1 : 0;
		UE_LOG(LogMOB, Log, TEXT("MOB_DEVICES found=%d launchers=%d teleports=%d shapesOff=%d lightsOff=%d glowOff=%d sounds=%d fx=%d break=%d insideSelf=%lld insideRemote=%lld overlapSelf=%lld overlapRemote=%lld"),
			Devices.Num(), Devices.Num() - Teleports, Teleports, DeviceShapesOff, DeviceLightsOff, DeviceGlowOff, DeviceSounds, DeviceFx, bBreakDevices ? 1 : 0, DeviceInsideSelf, DeviceInsideRemote, DeviceOverlapSelf, DeviceOverlapRemote);
	}
	// fails = 예비 가득 실패 소식(판 14 · L-4) · failBad = 그중 남 이름 · give ≠ Full(기대 0)
	UE_LOG(LogMOB, Log, TEXT("MOB_PADS pads=%d msgs=%lld takes=%lld readies=%lld syncs=%lld badIndex=%lld noActor=%lld bound=%lld heals=%lld healBeforeFix=%lld healMismatch=%lld healNoPawn=%lld stateChecks=%lld stateDiff=%lld lastStateTick=%u fails=%lld failBad=%lld"),
		Pads.Num(), PadMsgs, PadTakes, PadReadies, PadSyncs, PadBadIndex, PadNoActor, PadBound, PadHeals, PadHealBeforeFix, PadHealMismatch, PadHealNoPawn, PadStateChecks, PadStateDiff, LastStateTick, PadFails, PadFailBad);
	// 무기 받침 · 가방(W-4) — 판정은 Tools/LyraPilot/check_pads_bridge.py --weapons(서버 PADEV 줄과 견준다). 예측 줍기 칸은 봇 「BOT weapons」 줄과 같은 이름
	{
		const mo::CSelfPrediction::PickupCounters PickupFix = Core ? Core->Prediction.GetPickupCounters() : mo::CSelfPrediction::PickupCounters();
		UE_LOG(LogMOB, Log, TEXT("MOB_WEAPONPADS takesMine=%lld takesOther=%lld gives=%lld giveDiff=%lld cues=%lld noPawn=%lld bad=%lld bagStates=%lld bagDiffStates=%lld bagAdds=%lld bagRemoves=%lld bagStrays=%lld bag1s=%lld bagDiff1s=%lld invCompared=%lld invMulti=%lld invDiff=%lld ammoCompared=%lld ammoDiff=%lld received=%llu rerun=%llu noSlot=%llu resultDiffs=%llu ammoFixes=%llu meleeInWindow=%llu places=%lld noPad=%lld steps=%d failMine=%lld failGives=%lld failMsgs=%lld failDiff=%lld failNoPawn=%lld failMsgsElsewhere=%lld"),
			PadWeaponTakesMine, PadWeaponTakesOther, PadWeaponGives, PadWeaponGiveDiff, PadWeaponCues, PadWeaponNoPawn, PadWeaponBad, BagStates, BagDiffStates, BagAdds, BagRemoves, BagStrays,
			BagCheck1s, BagDiff1s, InvCompared, InvMulti, InvDiff, AmmoCompared, AmmoDiff,
			(unsigned long long)PickupFix.received, (unsigned long long)PickupFix.rerun, (unsigned long long)PickupFix.noSlot, (unsigned long long)PickupFix.resultDiffs, (unsigned long long)PickupFix.ammoFixes, (unsigned long long)PickupFix.meleeInWindow,
			WsPlaces, WsNoPad, WsStep, PadFailMine, PadFailGives, PadFailMsgs, PadFailDiff, PadFailNoPawn, PadFailMsgsElsewhere);
	}
	if (bHumanCheck) LogHumanCheck();
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
		// 사람 확인 — 꺼진 받침의 대기 표시 값(Lyra 가 읽는 CoolDownPercentage)을 1초마다. 정답지 = Lyra 기록 pads3 의 padBigPct · padSmallPct 흐름
		if (bHumanCheck && PercentProp && Now >= HcNextPadLogAt)
		{
			HcNextPadLogAt = Now + 1.0;
			for (int32 i = 0; i < Pads.Num(); ++i)
			{
				const ALyraWeaponSpawner* A = Pads[i].Actor.Get();
				if (!A || Pads[i].bAvailable) continue;
				UE_LOG(LogMOB, Log, TEXT("MOB_HC_PADPCT t=%.2f pad=%d pct=%.4f shown=%d serverLeft=%.2f cool=%u"), Now - ReadyAt, i, PercentProp->GetPropertyValue_InContainer(A),
					A->WeaponMesh && A->WeaponMesh->IsVisible() ? 1 : 0, ((double)Pads[i].ReadyTick - ServerTickNow) / wire::kTickRate, Pads[i].CooldownTicks);
			}
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

	// 1초마다 — 내 Lyra 빠른 칸 · 든 칸이 마지막 서버 상태의 가방 · 든 무기와 같은가(W-4). 상태마다 맞추므로(SyncSelfWeapon) 다르면 그 사이 Lyra 가 혼자 바꾼 것이다.
	// 죽은 동안은 쉰다(LastServerHeld 0xFF)
	if (LastServerHeld != 0xFF && bHasSpawn)
	{
		if (BreakBag & 4)
		{
			BreakBag &= ~4;
			APlayerController* PC = GetWorld()->GetFirstPlayerController();
			ULyraQuickBarComponent* Bar = PC ? PC->FindComponentByClass<ULyraQuickBarComponent>() : nullptr;
			if (Bar && Bar->GetActiveSlotIndex() != INDEX_NONE)
			{
				const int32 Slot = Bar->GetActiveSlotIndex();
				Bar->RemoveItemFromSlot(Slot);
				UE_LOG(LogMOB, Log, TEXT("MOB_BREAK_BAG removedActive slot=%d"), Slot);
			}
		}
		++BagCheck1s;
		if (!LyraBagMatches(LastServerQuickSlots, LastServerHeld))
		{
			++BagDiff1s;
			if (BagDiffLinesLeft > 0)
			{
				--BagDiffLinesLeft;
				int32 Active = INDEX_NONE;
				const FString Bag = DescribeLyraBag(&Active);
				UE_LOG(LogMOB, Warning, TEXT("MOB_BAG_DIFF_1S server=%u weapon=%u lyra=%s active=%d"), LastServerQuickSlots, LastServerHeld, *Bag, Active);
			}
		}
	}
}

// 점프대 · 순간이동 찾기 — 청사진 클래스라 C++ 헤더가 없다. 부모 줄을 따라 이름으로 가른다(B_Launcher_Up · B_Launcher_Push 는 B_Launcher_C 자식,
// ShooterCore 청사진 이름표 · L_Expanse 바깥 배우 = 위로 4 · 날리기 4 · 순간이동 6, 11-18 원본 기준)
void UMOBridgeSubsystem::FindDevices()
{
	static const FName LauncherName(TEXT("B_Launcher_C"));
	static const FName TeleportName(TEXT("B_Teleport_C"));
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		AActor* A = *It;
		int Kind = 0;		// 1 = 점프대 · 2 = 순간이동
		for (const UClass* C = A->GetClass(); C && !Kind; C = C->GetSuperClass())
		{
			if (C->GetFName() == LauncherName) Kind = 1;
			else if (C->GetFName() == TeleportName) Kind = 2;
		}
		if (!Kind) continue;
		bool bKnown = false;
		for (const FMOBDevice& D : Devices) bKnown |= D.Actor.Get() == A;
		if (bKnown) continue;

		FMOBDevice& D = Devices.AddDefaulted_GetRef();
		D.Actor = A;
		D.bTeleport = Kind == 2;
		TInlineComponentArray<UPrimitiveComponent*> Prims(A);
		for (UPrimitiveComponent* P : Prims)
		{
			if (!P->GetGenerateOverlapEvents()) continue;
			D.Shapes.Add(P);
			if (bBreakDevices) continue;
			// 겹침은 두 쪽이 다 켜야 난다 — 장치 쪽만 끄면 몸 캡슐은 그대로 두고 장치 그래프만 멈춘다
			P->SetGenerateOverlapEvents(false);
			// 끄기 전에 이미 든 겹침은 칸이 다음에 겹침을 다시 셀 때에야 끝난다(PrimitiveComponent.cpp UpdateOverlapsImpl 「just turned off」 갈래) — 장치는 안 움직여 그때가 안 오니 지금 센다
			P->UpdateOverlaps();
			++DeviceShapesOff;
		}
		// 점프대 겉모습도 끈다(사용자 「노란불빛 기능 이펙트 소리같은거 다 꺼」, 11-18). 노란 빛 = 바닥 조명(PointLight) + 발판 빛 재질
		// (MI_InternalShapeGlow 의 Intensity · Color — 청사진 짓기 그래프가 Light Color · Glow Intensity 로 채움, 에셋 이름표 기준).
		// 밟을 때 소리 · 번쩍임은 GameplayCue.World.Launcher.Activate 에서만 나서 위에서 겹침을 끈 것으로 이미 안 난다
		if (!D.bTeleport && !bBreakDevices)
		{
			static const FName GlowParam(TEXT("Intensity"));
			TInlineComponentArray<UActorComponent*> Parts(A);
			for (UActorComponent* Part : Parts)
			{
				if (ULightComponent* Light = Cast<ULightComponent>(Part))
				{
					Light->SetVisibility(false);
					++DeviceLightsOff;
				}
				else if (UAudioComponent* Sound = Cast<UAudioComponent>(Part))
				{
					Sound->Stop();
					Sound->SetAutoActivate(false);
					++DeviceSounds;
				}
				else if (UFXSystemComponent* Fx = Cast<UFXSystemComponent>(Part))
				{
					Fx->Deactivate();
					Fx->SetVisibility(false);
					++DeviceFx;
				}
				else if (UMeshComponent* Mesh = Cast<UMeshComponent>(Part))
				{
					for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
					{
						float Before = 0.f;
						const UMaterialInterface* Mat = Mesh->GetMaterial(Slot);
						if (!Mat || !Mat->GetScalarParameterValue(FHashedMaterialParameterInfo(GlowParam), Before)) continue;
						UMaterialInstanceDynamic* Mid = Mesh->CreateDynamicMaterialInstance(Slot);
						if (!Mid) continue;
						Mid->SetScalarParameterValue(GlowParam, 0.f);
						++DeviceGlowOff;
						UE_LOG(LogMOB, Log, TEXT("MOB_DEVICE_GLOW name=%s slot=%d mat=%s before=%.2f after=%.2f"), *A->GetName(), Slot, *Mat->GetName(), Before, Mid->K2_GetScalarParameterValue(GlowParam));
					}
				}
			}
		}
		UE_LOG(LogMOB, Log, TEXT("MOB_DEVICE name=%s class=%s shapes=%d at=(%.0f,%.0f,%.0f) off=%d"), *A->GetName(), *A->GetClass()->GetName(), D.Shapes.Num(),
			A->GetActorLocation().X, A->GetActorLocation().Y, A->GetActorLocation().Z, bBreakDevices ? 0 : 1);
	}
}

// 끈 판의 「겹침 0」이 「아무도 안 밟음」이 아님을 보이려고 지나간 양을 같이 센다 — 몸 캡슐(원기둥 반지름 · 반높이)이 장치 칸 상자에 닿은 프레임.
// 겹침 = 그 프레임에 Lyra 가 장치 칸 겹침 목록에 그 몸을 넣었나(IsOverlappingActor). 끈 판은 0, -MOBBreakDevices 판은 밟을 때마다 늘어야 한다
void UMOBridgeSubsystem::TickDevices(double Now)
{
	if (Devices.Num() == 0) return;
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const APawn* Self = PC ? PC->GetPawn() : nullptr;
	for (TActorIterator<ACharacter> It(GetWorld()); It; ++It)
	{
		const ACharacter* Body = *It;
		float Radius = 0, HalfHeight = 0;
		Body->GetSimpleCollisionCylinder(Radius, HalfHeight);
		const FVector Where = Body->GetActorLocation();
		const bool bSelf = Body == Self;
		for (const FMOBDevice& D : Devices)
		{
			for (const TWeakObjectPtr<UPrimitiveComponent>& Weak : D.Shapes)
			{
				const UPrimitiveComponent* Shape = Weak.Get();
				if (!Shape || !Shape->Bounds.GetBox().ExpandBy(FVector(Radius, Radius, HalfHeight)).IsInsideOrOn(Where)) continue;
				++(bSelf ? DeviceInsideSelf : DeviceInsideRemote);
				if (Shape->IsOverlappingActor(Body)) ++(bSelf ? DeviceOverlapSelf : DeviceOverlapRemote);
			}
		}
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
	const bool bFail = M.reason == wire::PICKUP_FAIL_FULL;
	if (bTake) ++PadTakes;
	else if (M.reason == wire::PICKUP_READY) ++PadReadies;
	else if (bFail)
	{
		// 예비 가득 실패(판 14 · L-4) — 서버는 집으려던 사람에게만 · give = Full 로 보낸다. 그 밖의 모양은 failBad
		++PadFails;
		if (M.picker != PlayerId || M.give != (uint8)mo::EGiveResult::Full) ++PadFailBad;
	}
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
	Pad.Kind = M.kind;
	ALyraWeaponSpawner* Actor = bKeepPickups ? nullptr : Pad.Actor.Get();
	if (!Actor && !bKeepPickups) Actor = BindPad(M.pad);	// 묶을 때 지금 상태로 그린다
	if (!Actor) ++PadNoActor;
	// 효과는 집음 · 다시 켜짐만 — 현황(들어올 때 · 경기 시작)은 상태만 맞춘다. Lyra 도 새로 놓은 받침은 효과 없이 켜진다.
	// 실패 소식은 받침이 그대로라 그리지 않는다(「예비 탄약 가득 참」 알림은 아래 ApplyWeaponPickupFail — L-9-a)
	if (Actor && !bFail) ApplyPadLook(*Actor, Pad.bAvailable, M.reason != wire::PICKUP_SYNC);
	// 받침 종류로 가른다(W-4) — 체력 받침은 회복, 무기 받침은 가방. 무기 받침 집음은 줍기 칸(give)이 늘 차 있다(서버 SendPad)
	const bool bHealPad = M.kind == (uint8)mo::EPadKind::Heal;
	const float LyraHp = bTake && !bKeepPickups && bHealPad ? ApplyPadHeal(M, Actor) : -1.f;
	if (bTake && !bKeepPickups && !bHealPad)
	{
		if (M.give == wire::kPickupGiveNone || M.weapon >= (uint8)mo::EWeapon::Count) ++PadWeaponBad;
		else ApplyWeaponPickup(M, Actor);
	}
	// 예비 가득 실패(L-9-a) — 내 것이면 받침 그래프를 내 몸에 불러 그래프 실패 길이 문구를 띄우게 한다. 모양이 틀린 소식(failBad)은 안 부른다
	if (bFail && !bKeepPickups && M.picker == PlayerId && M.give == (uint8)mo::EGiveResult::Full && M.weapon < (uint8)mo::EWeapon::Count) ApplyWeaponPickupFail(M, Actor);
	static const TCHAR* ReasonNames[] = { TEXT("take"), TEXT("takeRecheck"), TEXT("ready"), TEXT("sync"), TEXT("failFull") };	// EPickupReason 차례
	UE_LOG(LogMOB, Log, TEXT("MOB_PAD tick=%u pad=%u reason=%s avail=%u ready=%u picker=%u heal=%.1f hpAfter=%.1f lyraHp=%.1f actor=%d kind=%u give=%u weapon=%u slot=%u mag=%d spare=%d seq=%u"), M.tick, M.pad,
		M.reason < UE_ARRAY_COUNT(ReasonNames) ? ReasonNames[M.reason] : TEXT("?"), M.available, M.readyTick, M.picker, M.heal, M.hpAfter, LyraHp, Actor ? 1 : 0,
		M.kind, M.give, M.weapon, M.slot, (int32)M.magazine, (int32)M.spare, M.seq);
	// 사람 확인 — 내가 집은 받침: 집은 순간 · 대기 중 셋(대기의 1/4 · 2/4 · 3/4) · 다시 생긴 순간을 찍는다.
	// 준비 뒤 6초 안은 Lyra 로딩 화면이 남아 있어 안 찍는다(파일럿 ha1 — 2.28 · 3.10초 집음 사진이 로딩 화면. 6 은 제 판단)
	const bool bHcShotOk = bHumanCheck && ReadyAt > 0 && FPlatformTime::Seconds() - ReadyAt >= 6.0;
	if (bHumanCheck && bTake && M.picker == PlayerId)
	{
		HcMyPad = (int32)M.pad;
		const double Cool = (double)M.cooldownTicks / wire::kTickRate;
		if (bHcShotOk) RequestEventShot(TEXT("padtake"), 0.15, 2);
		if (bHcShotOk) for (int32 q = 1; q <= 3; ++q) RequestEventShot(TEXT("padcool"), Cool * q / 4.0, 6);
	}
	if (bHcShotOk && M.reason == wire::PICKUP_READY && (int32)M.pad == HcMyPad) RequestEventShot(TEXT("padready"), 0.3, 2);
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
	if (bAvailable)
	{
		Actor.PlayRespawnEffects();
		++PadRespawnEffects;
	}
	else
	{
		Actor.PlayPickupEffects();
		++PadPickupEffects;
	}
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

// 무기 아이템 탄 표식(Lyra 무기 아이템 ID_* 의 SetStats 조각이 처음 값을 넣는 그 표식)
static const FGameplayTag& MOBMagazineTag()
{
	static const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(TEXT("Lyra.ShooterGame.Weapon.MagazineAmmo"));
	return Tag;
}

static const FGameplayTag& MOBSpareTag()
{
	static const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(TEXT("Lyra.ShooterGame.Weapon.SpareAmmo"));
	return Tag;
}

// 받침 자기 무기 자료 — ALyraWeaponSpawner::WeaponDefinition 은 protected 라 속성 이름으로 읽는다(MOBContentProbeSubsystem.cpp PropText 와 같은 길)
static const ULyraPickupDefinition* MOBPadWeaponDefinition(const ALyraWeaponSpawner* Pad)
{
	static const FObjectPropertyBase* Prop = CastField<FObjectPropertyBase>(ALyraWeaponSpawner::StaticClass()->FindPropertyByName(TEXT("WeaponDefinition")));
	return Prop && Pad ? Cast<ULyraPickupDefinition>(Prop->GetObjectPropertyValue_InContainer(Pad)) : nullptr;
}

static void MOBSetItemStack(ULyraInventoryItemInstance& Item, const FGameplayTag& Stack, int32 Want)
{
	const int32 Have = Item.GetStatTagStackCount(Stack);
	if (Have < Want) Item.AddStatTagStack(Stack, Want - Have);
	else if (Have > Want) Item.RemoveStatTagStack(Stack, Have - Want);
}

// 무기 받침 집음(W-4) — 서버가 정한 결과(give · 칸 · 탄)를 Lyra 에 옮긴다.
// 내 몸: 예측에 같은 Give 를 넘기고(OnPickup — 소식은 그 틱 상태보다 먼저 온다), 받침 자기 그래프 GiveWeapon 을 부른다(Lyra 가 겹침으로 집을 때 부르는 그 함수 —
//   새 무기면 빈 첫 칸에 넣고 재장전 중(Event.Movement.Reload)이 아니면 든다 · 가진 무기면 예비를 기본값까지 채운다 · 효과 신호를 낸다).
//   그 뒤 Lyra 빠른 칸 · 든 칸 · 그 무기 탄을 소식과 견준다. 다르면 세고 탄만 소식대로 고친다 — 칸은 그 틱 상태의 가방 맞추기(SyncSelfWeapon)가 서버 값으로 고친다.
// 남의 몸: Lyra 는 남의 가방을 안 그린다(남은 든 무기만 보인다 — SyncRemoteWeapon) — 그래프가 내는 줍기 신호(효과 · 소리)만 그 몸에 튼다
void UMOBridgeSubsystem::ApplyWeaponPickup(const wire::Pickup& M, ALyraWeaponSpawner* Actor)
{
	static const FGameplayTag PickupCue = FGameplayTag::RequestGameplayTag(TEXT("GameplayCue.ShooterGame.Interact.WeaponPickup"));
	APawn* Pawn = FindPlayerPawn(M.picker);
	if (M.picker != PlayerId)
	{
		++PadWeaponTakesOther;
		if (!Pawn || !Actor)
		{
			++PadWeaponNoPawn;
			return;
		}
		FGameplayCueParameters Params;
		Params.Instigator = Actor;
		UGameplayCueFunctionLibrary::ExecuteGameplayCueOnActor(Pawn, PickupCue, Params);
		++PadWeaponCues;
		return;
	}

	++PadWeaponTakesMine;
	if (Core)
	{
		if (BreakBag & 2)
		{
			BreakBag &= ~2;
			UE_LOG(LogMOB, Log, TEXT("MOB_BREAK_BAG skippedPrediction tick=%u pad=%u seq=%u"), M.tick, M.pad, M.seq);
		}
		else
		{
			Core->Prediction.OnPickup(M.seq, M.reason == wire::PICKUP_TAKE, M.weapon, M.give, M.slot, M.magazine, M.spare);
		}
	}
	// 줍기로 든 무기가 바뀌었으면 보내는 무기 번호도 따른다 — 「앞 입력과 달라졌을 때만 바꾼다」(Loadout.h OnInputWeapon) 규칙이라 옛 번호를 계속 보내면
	// 되돌아가지는 않지만, 그 옛 무기 칸 키를 눌러도 번호가 안 바뀌어 못 돌아간다(프로브 RunWeapons 의 adopt 와 같은 일)
	if (M.give == (uint8)mo::EGiveResult::NewEquipped)
	{
		Weapon = M.weapon;
		AutoWeapon = M.weapon;
	}

	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	ULyraQuickBarComponent* Bar = PC ? PC->FindComponentByClass<ULyraQuickBarComponent>() : nullptr;
	const ULyraPickupDefinition* PadDef = MOBPadWeaponDefinition(Actor);
	if (!Pawn || !Bar || !PadDef)
	{
		++PadWeaponNoPawn;
		return;
	}
	LoadWeaponClasses();
	TSubclassOf<ULyraInventoryItemDefinition> GiveDef = PadDef->InventoryItemDefinition;
	if (BreakBag & 1)
	{
		BreakBag &= ~1;
		const int32 Wrong = ((int32)M.weapon + 1) % (int32)mo::EWeapon::Count;
		GiveDef = WeaponItemDefs.IsValidIndex(Wrong) ? WeaponItemDefs[Wrong].Get() : nullptr;
		UE_LOG(LogMOB, Log, TEXT("MOB_BREAK_BAG wrongGive tick=%u pad=%u weapon=%u gave=%d"), M.tick, M.pad, M.weapon, Wrong);
	}
	const int32 ActiveBefore = Bar->GetActiveSlotIndex();
	int32 ActiveDummy = INDEX_NONE;
	const FString BagBefore = DescribeLyraBag(&ActiveDummy);
	Actor->GiveWeapon(GiveDef, Pawn);
	++PadWeaponGives;

	// 견주기 — 소식의 칸에 그 무기가 하나만 있고, 새로 들었으면 든 칸 = 그 칸 · 아니면 든 칸 그대로(Lyra 그래프는 탄 채움 · 재장전 중 새 무기에 칸을 안 바꾼다), 탄 = 소식
	UClass* WantDef = WeaponItemDefs.IsValidIndex(M.weapon) ? WeaponItemDefs[M.weapon].Get() : nullptr;
	const TArray<ULyraInventoryItemInstance*> Slots = Bar->GetSlots();
	int32 Copies = 0;
	for (const ULyraInventoryItemInstance* Item : Slots) Copies += Item && Item->GetItemDef() == WantDef ? 1 : 0;
	ULyraInventoryItemInstance* Item = Slots.IsValidIndex(M.slot) ? Slots[M.slot] : nullptr;
	const bool bSlotOk = WantDef && Item && Item->GetItemDef() == WantDef && Copies == 1;
	const int32 WantActive = M.give == (uint8)mo::EGiveResult::NewEquipped ? (int32)M.slot : ActiveBefore;
	const int32 ActiveAfter = Bar->GetActiveSlotIndex();
	const int32 Mag = Item ? Item->GetStatTagStackCount(MOBMagazineTag()) : -1;
	const int32 Spare = Item ? Item->GetStatTagStackCount(MOBSpareTag()) : -1;
	const bool bAmmoOk = Mag == (int32)M.magazine && Spare == (int32)M.spare;
	int32 Active = INDEX_NONE;
	const FString BagAfter = DescribeLyraBag(&Active);
	UE_LOG(LogMOB, Log, TEXT("MOB_WEAPON_GIVE tick=%u pad=%u seq=%u give=%u weapon=%u slot=%u mag=%d spare=%d lyraBefore=%s active=%d lyraAfter=%s active=%d lyraMag=%d lyraSpare=%d ok=%d"),
		M.tick, M.pad, M.seq, M.give, M.weapon, M.slot, (int32)M.magazine, (int32)M.spare, *BagBefore, ActiveBefore, *BagAfter, ActiveAfter, Mag, Spare,
		bSlotOk && ActiveAfter == WantActive && bAmmoOk ? 1 : 0);
	if (bSlotOk && ActiveAfter == WantActive && bAmmoOk) return;
	++PadWeaponGiveDiff;
	if (WeaponGiveLinesLeft > 0)
	{
		--WeaponGiveLinesLeft;
		UE_LOG(LogMOB, Warning, TEXT("MOB_WEAPON_GIVE_DIFF tick=%u pad=%u give=%u weapon=%u slot=%u slotOk=%d copies=%d active=%d want=%d mag=%d/%d spare=%d/%d"),
			M.tick, M.pad, M.give, M.weapon, M.slot, bSlotOk ? 1 : 0, Copies, ActiveAfter, WantActive, Mag, (int32)M.magazine, Spare, (int32)M.spare);
	}
	if (bSlotOk && !bAmmoOk)
	{
		MOBSetItemStack(*Item, MOBMagazineTag(), M.magazine);
		MOBSetItemStack(*Item, MOBSpareTag(), M.spare);
	}
}

// 무기 받침 예비 가득 실패(L-9-a) — 집음(ApplyWeaponPickup)과 같은 길로 받침 그래프 GiveWeapon 을 내 몸에 부른다. 그래프는 가진 무기의 예비가 기본값 이상이면
// 실패 길(Broadcast Failure Message · Object = 받는 몸 → 거짓 반환)로 가고, 「예비 탄약 가득 참」 문구는 Lyra HUD 가 그 소식을 받아 띄운다(B_WeaponSpawner GiveWeapon 그래프 덤프).
// 실패 길을 지났나 = 부르는 동안 실패 채널에서 받은 내 PC 소식. 그 뒤 Lyra 탄이 서버 소식과 다르면(Lyra 예비가 덜 차 그래프가 채움 길로 감) 어긋남 — 서버 탄으로 되돌린다.
// 예측 · 가방 맞추기는 안 건드린다 — 실패는 서버 가방을 안 바꾼다(IsGiveTaken 거짓)
void UMOBridgeSubsystem::ApplyWeaponPickupFail(const wire::Pickup& M, ALyraWeaponSpawner* Actor)
{
	++PadFailMine;
	if (!FailMsgHandle.IsValid())
	{
		// 태그 변수(TAG_ABILITY_SIMPLE_FAILURE_MESSAGE)는 LyraGame 이 안 내보낸다 — 이름으로 찾는다(프로브 WatchWeapons 와 같음)
		static const FGameplayTag FailChannel = FGameplayTag::RequestGameplayTag(TEXT("Ability.UserFacingSimpleActivateFail.Message"));
		FailMsgHandle = UGameplayMessageSubsystem::Get(this).RegisterListener(FailChannel, this, &UMOBridgeSubsystem::OnFailMsg);
	}
	APawn* Pawn = FindPlayerPawn(M.picker);
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	ULyraQuickBarComponent* Bar = PC ? PC->FindComponentByClass<ULyraQuickBarComponent>() : nullptr;
	const ULyraPickupDefinition* PadDef = MOBPadWeaponDefinition(Actor);
	if (!Pawn || !Bar || !PadDef)
	{
		++PadFailNoPawn;
		return;
	}
	LoadWeaponClasses();
	UClass* WantDef = WeaponItemDefs.IsValidIndex(M.weapon) ? WeaponItemDefs[M.weapon].Get() : nullptr;
	// 소식의 칸(서버 SlotOf)에 그 무기가 있어야 한다 — 없으면 탄을 못 견줘 어긋남
	auto SlotItem = [&]() -> ULyraInventoryItemInstance*
	{
		const TArray<ULyraInventoryItemInstance*> Slots = Bar->GetSlots();
		ULyraInventoryItemInstance* It = Slots.IsValidIndex(M.slot) ? Slots[M.slot] : nullptr;
		return It && WantDef && It->GetItemDef() == WantDef ? It : nullptr;
	};
	ULyraInventoryItemInstance* Item = SlotItem();
	int32 SpareBefore = Item ? Item->GetStatTagStackCount(MOBSpareTag()) : -1;	// 부르기 직전 Lyra 예비(일부러 깨기 16 이면 줄인 뒤)
	if ((BreakBag & 16) && Item)
	{
		BreakBag &= ~16;
		--SpareBefore;
		MOBSetItemStack(*Item, MOBSpareTag(), SpareBefore);
		UE_LOG(LogMOB, Log, TEXT("MOB_BREAK_BAG lowSpare tick=%u pad=%u weapon=%u spare=%d"), M.tick, M.pad, M.weapon, SpareBefore);
	}
	bool bCalled = false;
	PadFailCallMsgs = 0;
	if (BreakBag & 8)
	{
		BreakBag &= ~8;
		UE_LOG(LogMOB, Log, TEXT("MOB_BREAK_BAG skippedFailGive tick=%u pad=%u weapon=%u"), M.tick, M.pad, M.weapon);
	}
	else
	{
		bInPadFailGive = true;
		Actor->GiveWeapon(PadDef->InventoryItemDefinition, Pawn);
		bInPadFailGive = false;
		bCalled = true;
		++PadFailGives;
	}
	Item = SlotItem();
	const int32 Mag = Item ? Item->GetStatTagStackCount(MOBMagazineTag()) : -1;
	const int32 Spare = Item ? Item->GetStatTagStackCount(MOBSpareTag()) : -1;
	const bool bAmmoOk = Mag == (int32)M.magazine && Spare == (int32)M.spare;
	const bool bOk = bCalled && PadFailCallMsgs == 1 && bAmmoOk;
	UE_LOG(LogMOB, Log, TEXT("MOB_PAD_FAIL tick=%u pad=%u weapon=%u slot=%u mag=%d spare=%d lyraSpareBefore=%d lyraMag=%d lyraSpare=%d called=%d msgs=%d ok=%d"),
		M.tick, M.pad, M.weapon, M.slot, (int32)M.magazine, (int32)M.spare, SpareBefore, Mag, Spare, bCalled ? 1 : 0, PadFailCallMsgs, bOk ? 1 : 0);
	// 문구 사진 — 프로브 W-0-b 가 실패 뒤 10 · 40 프레임(60fps)에 찍은 것과 같은 때. -MOBEventShots 일 때만(RequestEventShot)
	RequestEventShot(TEXT("padfail"), 10.0 / 60.0, 4);
	RequestEventShot(TEXT("padfail"), 40.0 / 60.0, 4);
	if (bOk) return;
	++PadFailDiff;
	if (Item && !bAmmoOk)
	{
		MOBSetItemStack(*Item, MOBMagazineTag(), M.magazine);
		MOBSetItemStack(*Item, MOBSpareTag(), M.spare);
	}
}

// 실패 채널 소식(L-9-a) — 받침 그래프 실패 길 · Lyra 능력 실패가 다 이 채널로 온다. 내 GiveWeapon 을 부르는 동안 온 내 PC 소식만 그래프 실패 길로 센다
void UMOBridgeSubsystem::OnFailMsg(FGameplayTag Channel, const FLyraAbilitySimpleFailureMessage& Msg)
{
	const bool bMine = Msg.PlayerController && Msg.PlayerController == GetWorld()->GetFirstPlayerController();
	if (bInPadFailGive && bMine)
	{
		++PadFailCallMsgs;
		++PadFailMsgs;
	}
	else ++PadFailMsgsElsewhere;
	UE_LOG(LogMOB, Log, TEXT("MOB_PAD_FAILMSG during=%d mine=%d pc=%s tags=%s reason=\"%s\""), bInPadFailGive ? 1 : 0, bMine ? 1 : 0, *GetNameSafe(Msg.PlayerController),
		*Msg.FailureTags.ToStringSimple(), *Msg.UserFacingReason.ToString());
}

FString UMOBridgeSubsystem::DescribeLyraBag(int32* OutActive) const
{
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const ULyraQuickBarComponent* Bar = PC ? PC->FindComponentByClass<ULyraQuickBarComponent>() : nullptr;
	if (OutActive) *OutActive = Bar ? Bar->GetActiveSlotIndex() : INDEX_NONE;
	if (!Bar) return TEXT("none");
	FString Out;
	for (const ULyraInventoryItemInstance* Item : Bar->GetSlots())
	{
		if (!Out.IsEmpty()) Out += TEXT(",");
		const int32 W = Item ? WeaponItemDefs.IndexOfByKey(Item->GetItemDef().Get()) : INDEX_NONE;
		Out += !Item ? FString(TEXT("-")) : W == INDEX_NONE ? FString(TEXT("?")) : FString::FromInt(W);
	}
	return Out;
}

bool UMOBridgeSubsystem::LyraBagMatches(uint8 PackedSlots, uint8 HeldWeapon) const
{
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const ULyraQuickBarComponent* Bar = PC ? PC->FindComponentByClass<ULyraQuickBarComponent>() : nullptr;
	if (!Bar) return false;
	const TArray<ULyraInventoryItemInstance*> Slots = Bar->GetSlots();
	int32 HeldSlot = INDEX_NONE;
	for (int32 S = 0; S < (int32)mo::kQuickSlotCount; ++S)
	{
		const uint8 W = mo::CLoadout::UnpackSlot(PackedSlots, (size_t)S);
		const UClass* Want = W < WeaponItemDefs.Num() ? WeaponItemDefs[W].Get() : nullptr;
		const ULyraInventoryItemInstance* Have = Slots.IsValidIndex(S) ? Slots[S] : nullptr;
		if ((Have ? Have->GetItemDef().Get() : nullptr) != Want) return false;
		if (W == HeldWeapon) HeldSlot = S;
	}
	return HeldSlot != INDEX_NONE && Bar->GetActiveSlotIndex() == HeldSlot;
}

void UMOBridgeSubsystem::SetLyraItemAmmo(uint8 WeaponIndex, int32 Magazine, int32 Spare)
{
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const ULyraQuickBarComponent* Bar = PC ? PC->FindComponentByClass<ULyraQuickBarComponent>() : nullptr;
	const UClass* Def = WeaponItemDefs.IsValidIndex(WeaponIndex) ? WeaponItemDefs[WeaponIndex].Get() : nullptr;
	if (!Bar || !Def) return;
	for (ULyraInventoryItemInstance* Item : Bar->GetSlots())
	{
		if (!Item || Item->GetItemDef() != Def) continue;
		MOBSetItemStack(*Item, MOBMagazineTag(), Magazine);
		MOBSetItemStack(*Item, MOBSpareTag(), Spare);
	}
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
		LastServerHeld = 0xFF;		// 1초 가방 대조를 쉰다 — 죽은 동안 Lyra 가 몸 · 장비를 정리한다
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

	// 가방(W-4) — Lyra 빠른 칸 셋을 서버 빠른 칸(quickSlots)과 같게 한다. 줍기는 받침 소식 때 Lyra 그래프가 먼저 넣었다(ApplyWeaponPickup) —
	// 여기는 그 밖의 것(부활로 [권총] 되돌림 · 놓친 소식 · Lyra 가 혼자 넣은 아이템)을 서버 값으로 되돌리고 센다. 빠른 칸 밖에 남은 무기 아이템도 지운다
	// (전에는 첫 판 범위 세 무기를 늘 다 넣어 뒀다 — 서버 가방이 생기기 전)
	++BagStates;
	bool bBagDiff = false;
	FString BagBefore;
	int32 ActiveBefore = INDEX_NONE;
	const auto NoteDiff = [&]()
	{
		if (bBagDiff) return;
		bBagDiff = true;
		BagBefore = DescribeLyraBag(&ActiveBefore);
	};
	int32 WantSlot = INDEX_NONE;
	for (int32 S = 0; S < (int32)mo::kQuickSlotCount; ++S)
	{
		const uint8 W = mo::CLoadout::UnpackSlot(P.quickSlots, (size_t)S);
		UClass* Want = W < WeaponItemDefs.Num() ? WeaponItemDefs[W].Get() : nullptr;
		if (W == P.weapon) WantSlot = S;
		if (W != mo::kNoWeapon && !Want) continue;		// 무기 클래스를 못 읽었다(MOB_WEAPON_CLASS item=0) — 건드리지 않는다
		const TArray<ULyraInventoryItemInstance*> Slots = Bar->GetSlots();
		ULyraInventoryItemInstance* Have = Slots.IsValidIndex(S) ? Slots[S] : nullptr;
		if ((Have ? Have->GetItemDef().Get() : nullptr) == Want) continue;
		NoteDiff();
		if (Have)
		{
			Bar->RemoveItemFromSlot(S);		// 든 칸이면 Lyra 가 내려놓고 든 칸을 비운다 — 아래 SetActiveSlotIndex 가 다시 든다
			Inv->RemoveItemInstance(Have);
			++BagRemoves;
		}
		if (Want)
		{
			if (ULyraInventoryItemInstance* Item = Inv->AddItemDefinition(Want, 1))
			{
				Bar->AddItemToSlot(S, Item);
				++BagAdds;
			}
		}
	}
	{
		const TArray<ULyraInventoryItemInstance*> SlotsNow = Bar->GetSlots();
		for (ULyraInventoryItemInstance* Item : Inv->GetAllItems())
		{
			if (!Item || !WeaponItemDefs.Contains(Item->GetItemDef().Get()) || SlotsNow.Contains(Item)) continue;
			NoteDiff();
			Inv->RemoveItemInstance(Item);
			++BagStrays;
		}
	}
	if (bBagDiff)
	{
		++BagDiffStates;
		if (BagDiffLinesLeft > 0)
		{
			--BagDiffLinesLeft;
			int32 ActiveAfter = INDEX_NONE;
			const FString BagAfter = DescribeLyraBag(&ActiveAfter);
			UE_LOG(LogMOB, Log, TEXT("MOB_BAG_DIFF seq=%u server=%u weapon=%u lyraBefore=%s active=%d lyraAfter=%s active=%d sinceSpawn=%.2f"), P.lastSeq, P.quickSlots, P.weapon,
				*BagBefore, ActiveBefore, *BagAfter, ActiveAfter, LastSelfSpawnAt > 0 ? FPlatformTime::Seconds() - LastSelfSpawnAt : -1.0);
		}
	}
	LastServerQuickSlots = P.quickSlots;
	LastServerHeld = P.weapon;
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
