#include "MOBContentProbeSubsystem.h"

#include "MOBLyraCamera.h"
#include "MOBSimpleAnimInstance.h"

#include "AIController.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "BrainComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Kismet/KismetSystemLibrary.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"

#include "AbilitySystem/Attributes/LyraHealthSet.h"
#include "AbilitySystem/LyraAbilitySystemComponent.h"
#include "Camera/LyraCameraMode.h"
#include "Character/LyraCharacter.h"
#include "GameFramework/GameStateBase.h"
#include "Player/LyraPlayerState.h"
#include "Equipment/LyraEquipmentDefinition.h"
#include "Equipment/LyraEquipmentInstance.h"
#include "Equipment/LyraEquipmentManagerComponent.h"
#include "Physics/LyraCollisionChannels.h"
#include "Teams/LyraTeamSubsystem.h"
#include "Weapons/LyraWeaponSpawner.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MOBContentProbeSubsystem)

DEFINE_LOG_CATEGORY_STATIC(LogMOBCProbe, Log, All);

using EAct = UMOBContentProbeSubsystem::EAct;

namespace
{
	struct FStep
	{
		const TCHAR* Name;
		float Seconds;
		EAct Act;
		float Arg;
		bool bWalk;		// 칸 내내 +X 로 걷는다
	};

	// 연습 판 — 애니 비교 촬영의 고정 평지(MOBAnimCompareSubsystem 의 FIXED_HOME). 그 촬영이 여기서 +X 로 2초 달린 길이라 1.2초 걷기는 막히지 않는다
	const FVector GYM_HOME(-1020.0, -1070.0, 142.0);

	// 근접 거리(내 중심 → 상대 중심, cm). 에셋 값으로 맞는 끝 = 손 위치 + 350 + 쓸기 반지름 96 + 상대 캡슐 35 ≈ 손 + 481 → 그 둘레를 촘촘히
	const FStep GYM_SCRIPT[] = {
		{ TEXT("settle"),          1.0f, EAct::Home,         0.f,    false },
		// 조준 — 서서 켜고 끄기(섞임 곡선) · 걸으며(걸음 속도)
		{ TEXT("ads_on"),          1.0f, EAct::AdsPress,     0.f,    false },
		{ TEXT("ads_off"),         1.0f, EAct::AdsRelease,   0.f,    false },
		{ TEXT("walk_home"),       0.5f, EAct::Home,         0.f,    false },
		{ TEXT("walk"),            1.2f, EAct::None,         0.f,    true  },
		{ TEXT("ads_walk"),        1.2f, EAct::AdsPress,     0.f,    true  },
		{ TEXT("ads_walk_off"),    1.0f, EAct::AdsRelease,   0.f,    true  },
		// 앉기 — 앉은 캡슐 · 눈높이 · 걸음 속도 · 앉아 조준한 걸음 속도
		{ TEXT("crouch_home"),     0.5f, EAct::Home,         0.f,    false },
		{ TEXT("crouch"),          1.0f, EAct::CrouchToggle, 0.f,    false },
		{ TEXT("crouch_walk"),     1.2f, EAct::None,         0.f,    true  },
		{ TEXT("crouch_ads_walk"), 1.2f, EAct::AdsPress,     0.f,    true  },
		{ TEXT("crouch_ads_off"),  0.5f, EAct::AdsRelease,   0.f,    false },
		{ TEXT("uncrouch"),        1.0f, EAct::CrouchToggle, 0.f,    false },
		// 근접 — 거리마다 한 번(맞음 · 피해 · 판정 시각 · 밀기 · 맞은 쪽 몽타주)
		{ TEXT("foe_setup"),       0.5f, EAct::FoeSetup,     0.f,    false },
		{ TEXT("melee_150"),       1.5f, EAct::MeleeAt,      150.f,  false },
		{ TEXT("melee_300"),       1.5f, EAct::MeleeAt,      300.f,  false },
		{ TEXT("melee_400"),       1.5f, EAct::MeleeAt,      400.f,  false },
		{ TEXT("melee_450"),       1.5f, EAct::MeleeAt,      450.f,  false },
		{ TEXT("melee_480"),       1.5f, EAct::MeleeAt,      480.f,  false },
		{ TEXT("melee_500"),       1.5f, EAct::MeleeAt,      500.f,  false },
		{ TEXT("melee_520"),       1.5f, EAct::MeleeAt,      520.f,  false },
		{ TEXT("melee_550"),       1.5f, EAct::MeleeAt,      550.f,  false },
		{ TEXT("melee_600"),       1.5f, EAct::MeleeAt,      600.f,  false },
		{ TEXT("melee_back_200"),  1.5f, EAct::MeleeAtBack,  200.f,  false },
		// 근접 옆 거리 — 앞 250 에 세우고 옆(Y)으로 민다. 앞뒤 거리만으로는 손 위치와 쓸기 반지름이 한 식에 묶여 따로 안 나온다(gym1).
		// 에셋 값이면 |옆 − 손의 옆 위치| ≤ 96 + 35 = 131 에서 맞는다 → 양쪽 경계의 차 ÷ 2 가 반지름 합, 가운데가 손의 옆 위치
		{ TEXT("side_p100"),       1.5f, EAct::MeleeAtSide,  100.f,  false },
		{ TEXT("side_p115"),       1.5f, EAct::MeleeAtSide,  115.f,  false },
		{ TEXT("side_p130"),       1.5f, EAct::MeleeAtSide,  130.f,  false },
		{ TEXT("side_p145"),       1.5f, EAct::MeleeAtSide,  145.f,  false },
		{ TEXT("side_p160"),       1.5f, EAct::MeleeAtSide,  160.f,  false },
		{ TEXT("side_m100"),       1.5f, EAct::MeleeAtSide, -100.f,  false },
		{ TEXT("side_m115"),       1.5f, EAct::MeleeAtSide, -115.f,  false },
		{ TEXT("side_m130"),       1.5f, EAct::MeleeAtSide, -130.f,  false },
		{ TEXT("side_m145"),       1.5f, EAct::MeleeAtSide, -145.f,  false },
		{ TEXT("side_m160"),       1.5f, EAct::MeleeAtSide, -160.f,  false },
		// 17-d — 걸으며(칸 내내 +X 누름) · 뛰며(칸 첫 프레임 입력으로 점프, 0.3초 뒤 공중에서 누름).
		// 밀기 중 누름 · 낙하 갈래의 Lyra 값이 E-1 기록에 없었다(content-add-progress.md E-1 · E-2-1 「아직 안 본 것」). 벽 너머 칸은 미결(17-d d3 결과)
		{ TEXT("melee_walk_300"),  1.5f, EAct::MeleeAt,      300.f,  true  },
		{ TEXT("melee_walk_520"),  1.5f, EAct::MeleeAt,      520.f,  true  },
		{ TEXT("melee_jump_300"),  1.5f, EAct::MeleeJumpAt,  300.f,  false },
		// 400 — 450 은 맞은 거리 349.28 로 쓸기 끝에서 0.72cm 안이라 공중 손 1~3cm 차이에 결과가 갈렸다(17-d d3c)
		{ TEXT("melee_jump_400"),  1.5f, EAct::MeleeJumpAt,  400.f,  false },
		// 같은 팀 — GA_Melee 는 팀이 다를 때만 피해 · 밀기로 간다(에셋 기준). 같은 팀으로 바꿔 한 번, 다시 다른 팀으로
		{ TEXT("foe_same"),        0.5f, EAct::FoeSameTeam,  0.f,    false },
		{ TEXT("melee_same_300"),  1.5f, EAct::MeleeAt,      300.f,  false },
		{ TEXT("foe_setup2"),      0.5f, EAct::FoeSetup,     0.f,    false },
		// 근접 간격 — 무기마다 칸 내내 눌러 능력이 다시 켜지는 시각을 잰다(상대는 등 뒤 멀리)
		{ TEXT("eq_pistol"),       1.5f, EAct::Equip,        0.f,    false },
		{ TEXT("spam_pistol"),     3.0f, EAct::MeleeSpam,    0.f,    false },
		{ TEXT("eq_rifle"),        1.5f, EAct::Equip,        1.f,    false },
		{ TEXT("spam_rifle"),      3.0f, EAct::MeleeSpam,    0.f,    false },
		{ TEXT("eq_shotgun"),      1.5f, EAct::Equip,        2.f,    false },
		{ TEXT("spam_shotgun"),    3.0f, EAct::MeleeSpam,    0.f,    false },
		// E-1 — 무기 · 앉음마다 손 위치가 다르다. 같은 거리에서 한 번씩(쓸기 시작점을 무기 셋 · 앉음으로 넓힌다)
		{ TEXT("melee_shotgun_300"), 1.5f, EAct::MeleeAt,    300.f,  false },
		{ TEXT("eq_rifle2"),       1.5f, EAct::Equip,        1.f,    false },
		{ TEXT("melee_rifle_300"), 1.5f, EAct::MeleeAt,      300.f,  false },
		{ TEXT("eq_pistol2"),      1.5f, EAct::Equip,        0.f,    false },
		{ TEXT("crouch2"),         1.0f, EAct::CrouchToggle, 0.f,    false },
		{ TEXT("melee_crouch_300"), 1.5f, EAct::MeleeAt,     300.f,  false },
		{ TEXT("melee_crouch_500"), 1.5f, EAct::MeleeAt,     500.f,  false },
		{ TEXT("uncrouch2"),       1.0f, EAct::CrouchToggle, 0.f,    false },
		{ TEXT("end"),             0.5f, EAct::Home,         0.f,    false },
	};

	// 받침 판 — 맵 좌표(content-add-progress.md 0-3 받침 표의 B_AbilitySpawner3 = 큰 것, 1 = 작은 것). Arg 0 = 큰 것, 1 = 작은 것
	const FVector PAD_SPOTS[2] = { FVector(0.0, 0.0, -1218.1), FVector(0.0, -1086.0, 365.5) };

	const FStep PADS_SCRIPT[] = {
		{ TEXT("settle"),      1.0f,  EAct::PadsDump,  0.f,  false },
		{ TEXT("big_full"),    2.0f,  EAct::PadOn,     0.f,  false },	// 가득 찬 체력 — 받침이 남아야 한다(에셋 기준)
		{ TEXT("away1"),       1.0f,  EAct::Away,      0.f,  false },
		{ TEXT("hp40"),        0.5f,  EAct::SetHealth, 40.f, false },
		{ TEXT("big_hurt"),    52.0f, EAct::PadOn,     0.f,  false },	// +50 → 25초 뒤 다시 생김 → 0.25초 뒤 서 있는 나를 다시 본다 → 다시 검사로 집은 뒤의 대기도(B-2, 27 → 52)
		{ TEXT("away2"),       1.0f,  EAct::Away,      0.f,  false },
		{ TEXT("hp40b"),       0.5f,  EAct::SetHealth, 40.f, false },
		{ TEXT("small_hurt"),  22.0f, EAct::PadOn,     1.f,  false },	// +25 → 10초 뒤 → 다시 검사로 집은 뒤의 대기도(B-2, 12 → 22)
		{ TEXT("end"),         0.5f,  EAct::Away,      0.f,  false },
	};

	// 다리 판 — 지금 클라(내 서버에 붙은 Lyra)에서 세 키를 누르면 무슨 일이 나는지(content-add-plan.md 0-2). 순간이동 · 체력 조작 없이 누르기만 한다.
	// 마지막 칸은 봇끼리 싸우는 동안 점수판 칸(처치 · 죽음 · 도움)이 어떻게 쌓이는지 본다
	const FStep BRIDGE_SCRIPT[] = {
		{ TEXT("settle"),      1.0f,  EAct::None,         0.f, false },
		{ TEXT("ads_on"),      1.5f,  EAct::AdsPress,     0.f, false },
		{ TEXT("ads_off"),     1.0f,  EAct::AdsRelease,   0.f, false },
		{ TEXT("crouch"),      1.5f,  EAct::CrouchToggle, 0.f, false },
		{ TEXT("uncrouch"),    1.0f,  EAct::CrouchToggle, 0.f, false },
		{ TEXT("melee"),       1.5f,  EAct::MeleeHere,    0.f, false },
		{ TEXT("watch"),       45.0f, EAct::Watch,        0.f, false },
	};

	// 부딪힘 판(F-1, content-add-plan.md F 1) — 막히는지 미끄러지는지. 연습장 집 자리에서 나는 칸 내내 +X 로 걷는다.
	// Still = 상대는 서 있다 · 없으면 상대도 -X 로 걷는다(마주 걷기). At 의 Arg = 앞 거리, SideAt 의 Arg = 옆으로 어긋난 거리(앞 300).
	// 옆 20 은 두 반지름(70) 안쪽 깊게, 50 은 얕게 스친다. 2초 = 걷기 400cm/s 로 300 을 좁힌 뒤 1초 넘게 밀고 있는 시간
	constexpr float BUMP_FORWARD = 300.f;
	const FStep BUMP_SCRIPT[] = {
		{ TEXT("settle"),             1.0f, EAct::Home,            0.f,   false },
		{ TEXT("foe_setup"),          0.5f, EAct::FoeSetup,        0.f,   false },
		{ TEXT("bump_still_300"),     2.0f, EAct::BumpStillAt,     300.f, true  },
		{ TEXT("bump_head_300"),      2.0f, EAct::BumpAt,          300.f, true  },
		{ TEXT("bump_still_p20"),     2.0f, EAct::BumpStillSideAt, 20.f,  true  },
		{ TEXT("bump_still_p50"),     2.0f, EAct::BumpStillSideAt, 50.f,  true  },
		{ TEXT("bump_side_p20"),      2.0f, EAct::BumpSideAt,      20.f,  true  },
		{ TEXT("bump_side_p50"),      2.0f, EAct::BumpSideAt,      50.f,  true  },
		{ TEXT("bump_side_m50"),      2.0f, EAct::BumpSideAt,     -50.f,  true  },
		// 앉은 상대 · 앉은 나(D 뒤) — 앉은 캡슐은 반지름이 같고 반높이만 줄어든다(D 기록)
		{ TEXT("foe_crouch"),         1.0f, EAct::FoeCrouchToggle, 0.f,   false },
		{ TEXT("bump_crouched_300"),  2.0f, EAct::BumpStillAt,     300.f, true  },
		{ TEXT("bump_crouched_p50"),  2.0f, EAct::BumpStillSideAt, 50.f,  true  },
		{ TEXT("foe_uncrouch"),       1.0f, EAct::FoeCrouchToggle, 0.f,   false },
		{ TEXT("crouch_me"),          1.0f, EAct::CrouchToggle,    0.f,   false },
		{ TEXT("bump_me_crouched"),   2.0f, EAct::BumpStillAt,     300.f, true  },
		{ TEXT("uncrouch_me"),        1.0f, EAct::CrouchToggle,    0.f,   false },
		{ TEXT("end"),                0.5f, EAct::Home,            0.f,   false },
	};

	const TCHAR* const WEAPON_DEFS[3] = {
		TEXT("/ShooterCore/Weapons/Pistol/WID_Pistol.WID_Pistol_C"),
		TEXT("/ShooterCore/Weapons/Rifle/WID_Rifle.WID_Rifle_C"),
		TEXT("/ShooterCore/Weapons/Shotgun/WID_Shotgun.WID_Shotgun_C"),
	};

	constexpr float MATCH_SETTLE_SECONDS = 3.f;	// 몸 맞힘 촬영과 같은 절차 — 경기 시작 때 캐릭터가 한 번 바뀐다
	constexpr double WAIT_LIMIT_SECONDS = 120.0;
	constexpr double REAL_LIMIT_SECONDS = 400.0;
	// GA_Melee 기본 객체 값(0-3 표 · GA_Melee_cdo.t3d) — E-1 시험판이 같은 쓸기를 해 볼 때 쓴다
	constexpr float MELEE_TRACE_DISTANCE = 350.f;
	constexpr float MELEE_TRACE_RADIUS = 96.f;
	constexpr float MELEE_TRACE_HALF_HEIGHT = 48.f;
	constexpr float MELEE_PRESS_AT = 0.3f;		// 상대를 세운 뒤 누르기까지 — 순간이동 뒤 한 프레임 넘게 둔다(제안값)
	constexpr float BRIDGE_SETTLE_SECONDS = 5.f;	// 다리 판 — 캐릭터가 생긴 뒤 서버 위치를 받아 자리 잡을 때까지(제안값, run_game 의 준비 2초보다 길게)
	constexpr float MELEE_SIDE_FORWARD = 250.f;	// 옆 거리 칸에서 상대를 세우는 앞 거리 — 맞는 앞 범위(150~520, gym1) 한가운데쯤
	constexpr float WATCH_LOG_EVERY = 5.f;		// 다리 판 마지막 칸에서 점수를 적는 간격(초)

	// 판 번호 0 = 연습 · 1 = 받침 · 2 = 다리 · 3 = 부딪힘(연습장)
	const FStep& StepAt(int32 Script, int32 I)
	{
		switch (Script)
		{
		case 1: return PADS_SCRIPT[I];
		case 2: return BRIDGE_SCRIPT[I];
		case 3: return BUMP_SCRIPT[I];
		default: return GYM_SCRIPT[I];
		}
	}
	int32 StepCount(int32 Script)
	{
		switch (Script)
		{
		case 1: return UE_ARRAY_COUNT(PADS_SCRIPT);
		case 2: return UE_ARRAY_COUNT(BRIDGE_SCRIPT);
		case 3: return UE_ARRAY_COUNT(BUMP_SCRIPT);
		default: return UE_ARRAY_COUNT(GYM_SCRIPT);
		}
	}

	UAbilitySystemComponent* FindASC(const AActor* A)
	{
		return A ? UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(A) : nullptr;
	}

	// 조준 섞임 — Lyra 사격이 읽는 값 그대로(LyraRangedWeaponInstance.cpp 의 GetBlendInfo → 스택 맨 끝 모드의 비율 · 표식).
	// 사격이 읽는 쪽(스택 맨 끝)의 비중과 그것이 조준 카메라인지 — 읽는 법은 MOBLyraCamera.h
	bool ReadAimCamera(const APawn* Me, float& OutWeight, bool& bOutAds)
	{
		MOBLyraCamera::FRead R;
		const bool bOk = MOBLyraCamera::Read(Me, R);
		OutWeight = bOk ? R.BottomWeight : -1.f;
		bOutAds = bOk && R.bBottomAds;
		return bOk;
	}

	FString MontageOf(const ACharacter* C, float* OutPos = nullptr)
	{
		const UAnimInstance* AI = C && C->GetMesh() ? C->GetMesh()->GetAnimInstance() : nullptr;
		const UAnimMontage* M = AI ? AI->GetCurrentActiveMontage() : nullptr;
		if (OutPos) *OutPos = M ? AI->Montage_GetPosition(M) : -1.f;
		return M ? M->GetName() : FString();
	}

	// 근접 쓸기 시작점 — GA_Melee 가 읽는 그대로(메시 GetSocketLocation("hand_r"))
	FVector HandOf(const ACharacter* C)
	{
		return C && C->GetMesh() ? C->GetMesh()->GetSocketLocation(TEXT("hand_r")) : FVector::ZeroVector;
	}

	// 물리 몸 요소마다 세상 자리 — 다리 LogViewParts 와 같은 차례 · 같은 식(뼈 자리 × 요소 자리, 크기 1)
	void CollectParts(const ACharacter* C, TArray<FTransform>& Out, TArray<FName>& Names)
	{
		Out.Reset();
		Names.Reset();
		const USkeletalMeshComponent* Mesh = C ? C->GetMesh() : nullptr;
		const UPhysicsAsset* PA = Mesh ? Mesh->GetPhysicsAsset() : nullptr;
		if (!PA) return;
		for (const USkeletalBodySetup* B : PA->SkeletalBodySetups)
		{
			if (!B) continue;
			const int32 BoneIndex = Mesh->GetBoneIndex(B->BoneName);
			if (BoneIndex == INDEX_NONE) continue;
			FTransform BoneW = Mesh->GetBoneTransform(BoneIndex);
			BoneW.SetScale3D(FVector::OneVector);
			for (const FKSphereElem& E : B->AggGeom.SphereElems) { Out.Add(FTransform(E.Center) * BoneW); Names.Add(B->BoneName); }
			for (const FKSphylElem& E : B->AggGeom.SphylElems) { Out.Add(FTransform(E.Rotation, E.Center) * BoneW); Names.Add(B->BoneName); }
			for (const FKBoxElem& E : B->AggGeom.BoxElems) { Out.Add(FTransform(E.Rotation, E.Center) * BoneW); Names.Add(B->BoneName); }
		}
	}

	FString PropText(const UObject* Obj, const TCHAR* Name)
	{
		const FProperty* P = Obj ? Obj->GetClass()->FindPropertyByName(Name) : nullptr;
		if (!P) return TEXT("?");
		FString S;
		P->ExportText_InContainer(0, S, Obj, nullptr, nullptr, PPF_None);
		return S;
	}
}

bool UMOBContentProbeSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* W = Cast<UWorld>(Outer);
	if (!W || !W->IsGameWorld()) return false;
	FString T;
	return FParse::Value(FCommandLine::Get(), TEXT("MOBContentProbe="), T);
}

void UMOBContentProbeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	const TCHAR* Cmd = FCommandLine::Get();
	FParse::Value(Cmd, TEXT("MOBContentProbe="), Tag);
	FString Mode;
	FParse::Value(Cmd, TEXT("MOBCPMode="), Mode);
	bPads = Mode == TEXT("pads");
	bBridge = Mode == TEXT("bridge");
	bBump = Mode == TEXT("bump");
	bForceQuinn = FParse::Param(Cmd, TEXT("MOBCPQuinn"));
	StartRealAt = FPlatformTime::Seconds();
	GAreScreenMessagesEnabled = false;
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_INIT world=%s tag=%s mode=%s"), *GetNameSafe(GetWorld()), *Tag, bBridge ? TEXT("bridge") : (bPads ? TEXT("pads") : (bBump ? TEXT("bump") : TEXT("gym"))));
}

TStatId UMOBContentProbeSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMOBContentProbeSubsystem, STATGROUP_Tickables);
}

ACharacter* UMOBContentProbeSubsystem::Me() const
{
	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	return PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
}

void UMOBContentProbeSubsystem::OnPawnHit(AActor* SelfActor, AActor* OtherActor, FVector NormalImpulse, const FHitResult& Hit)
{
	// 시각은 물림(PullBackHit) 뒤 값, 자리(loc)는 물림 전 엔진이 찾은 닿는 자리다. frame 은 기록 표 frame 칸과 같은 프레임
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_HIT frame=%d step=%s who=%s other=%s time=%.7f dist=%.7f pen=%d normal=%.8f,%.8f,%.8f impactN=%.8f,%.8f,%.8f start=%.10f,%.10f,%.10f end=%.10f,%.10f,%.10f loc=%.10f,%.10f,%.10f otherLoc=%.10f,%.10f,%.10f otherYaw=%.8f myYaw=%.8f"),
		Frame, bReady ? StepAt(ScriptIdx(), Step).Name : TEXT("-"), SelfActor == Me() ? TEXT("me") : TEXT("foe"), *GetNameSafe(OtherActor), Hit.Time, Hit.Distance, Hit.bStartPenetrating ? 1 : 0,
		Hit.Normal.X, Hit.Normal.Y, Hit.Normal.Z, Hit.ImpactNormal.X, Hit.ImpactNormal.Y, Hit.ImpactNormal.Z,
		Hit.TraceStart.X, Hit.TraceStart.Y, Hit.TraceStart.Z, Hit.TraceEnd.X, Hit.TraceEnd.Y, Hit.TraceEnd.Z, Hit.Location.X, Hit.Location.Y, Hit.Location.Z,
		OtherActor ? OtherActor->GetActorLocation().X : 0.0, OtherActor ? OtherActor->GetActorLocation().Y : 0.0, OtherActor ? OtherActor->GetActorLocation().Z : 0.0,
		OtherActor ? OtherActor->GetActorRotation().Yaw : 0.0, SelfActor ? SelfActor->GetActorRotation().Yaw : 0.0);
}

void UMOBContentProbeSubsystem::Tick(float DeltaTime)
{
	if (bDone) return;
	if (FPlatformTime::Seconds() - StartRealAt > REAL_LIMIT_SECONDS)
	{
		UE_LOG(LogMOBCProbe, Error, TEXT("MOB_CPROBE_FAIL realLimit step=%d"), Step);
		Finish(false);
		return;
	}
	if (!bReady)
	{
		if (!WaitReady(DeltaTime))
		{
			WaitSeconds += DeltaTime;
			if (WaitSeconds > WAIT_LIMIT_SECONDS)
			{
				UE_LOG(LogMOBCProbe, Error, TEXT("MOB_CPROBE_FAIL notReady"));
				Finish(false);
			}
			return;
		}
		if (!bPrepared)
		{
			if (!Prepare())
			{
				Finish(false);
				return;
			}
			bPrepared = true;
		}
		if (bForceQuinn && !QuinnReady(DeltaTime)) return;
		LogBodies(TEXT("start"));
		bReady = true;
		Step = 0;
		StepTime = 0.f;
		BeginStep();
	}

	// 도중에 내 캐릭터가 바뀌면(경기 시작 초기화 · 죽음) 그 뒤 값은 다른 판의 것이다 — 멈춘다. pads1 에서 경기 시작 초기화가 큰 받침 대기를 끊었다
	// 다리 판은 봇에게 죽어 다시 생기는 것도 지금 클라의 모습이라 적기만 하고 이어 간다
	if (Me() != ClearPawn.Get())
	{
		UE_LOG(LogMOBCProbe, Warning, TEXT("MOB_CPROBE_PAWN_CHANGED step=%d from=%s to=%s"), Step, *GetNameSafe(ClearPawn.Get()), *GetNameSafe(Me()));
		if (!bBridge)
		{
			Finish(false);
			return;
		}
		ClearPawn = Me();
		if (!ClearPawn.IsValid()) return;
	}

	if (bBump && !bHitsBound && Me() && Foe.IsValid())
	{
		Me()->OnActorHit.AddDynamic(this, &UMOBContentProbeSubsystem::OnPawnHit);
		Foe->OnActorHit.AddDynamic(this, &UMOBContentProbeSubsystem::OnPawnHit);
		bHitsBound = true;
	}
	DuringStep();
	WriteRow();
	StepTime += DeltaTime;
	++Frame;
	if (StepTime + KINDA_SMALL_NUMBER >= StepAt(ScriptIdx(), Step).Seconds)
	{
		EndStep();
		if (++Step >= StepCount(ScriptIdx()))
		{
			Finish(true);
			return;
		}
		StepTime = 0.f;
		BeginStep();
	}
}

// 내 캐릭터가 생기고 생성 무적이 풀린 채 3초 — 몸 맞힘 촬영과 같은 기준. 무적은 경기 전 준비 시간 내내 걸려 있어서, 이 기다림이 경기 시작 초기화(캐릭터 · 받침이 새로 생긴다)를 지나게 해 준다.
// 받침 판은 처음에 이 기다림을 뺐다가 초기화에 걸렸다(pads1, 시작 3.3초 → 초기화 31.4초)
bool UMOBContentProbeSubsystem::WaitReady(float DeltaTime)
{
	ACharacter* Ch = Me();
	const UAbilitySystemComponent* ASC = FindASC(Ch);
	static const FGameplayTag Immune = FGameplayTag::RequestGameplayTag(TEXT("Gameplay.DamageImmunity"));
	// 다리 판은 Lyra 경기 흐름을 다리가 막아(BlockLyraReset) 무적이 언제 풀리는지 정해져 있지 않다 → 캐릭터만 보고 더 오래 기다린다(서버 접속 · 첫 위치 받기)
	const bool bImmuneBlocks = !bBridge && ASC && ASC->HasMatchingGameplayTag(Immune);
	if (!ASC || bImmuneBlocks || Ch != ClearPawn.Get())
	{
		ClearPawn = Ch;
		ClearSeconds = 0.f;
		return false;
	}
	ClearSeconds += DeltaTime;
	return ClearSeconds > (bBridge ? BRIDGE_SETTLE_SECONDS : MATCH_SETTLE_SECONDS);
}

bool UMOBContentProbeSubsystem::Prepare()
{
	UWorld* World = GetWorld();
	ACharacter* Ch = Me();
	UCharacterMovementComponent* M = Ch ? Ch->GetCharacterMovement() : nullptr;
	if (!M) return false;

	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_PARAMS maxWalk=%.1f maxWalkCrouched=%.1f crouchedHalfHeight=%.1f crouchedEye=%.1f baseEye=%.1f capsuleR=%.1f capsuleHH=%.1f canWalkOffLedgesCrouched=%d hp=%.1f"),
		M->MaxWalkSpeed, M->MaxWalkSpeedCrouched, M->GetCrouchedHalfHeight(), Ch->CrouchedEyeHeight, Ch->BaseEyeHeight,
		Ch->GetCapsuleComponent()->GetScaledCapsuleRadius(), Ch->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), M->bCanWalkOffLedgesWhenCrouching ? 1 : 0, HealthOf(Ch));

	if (bPads || bBridge)
	{
		Home = Ch->GetActorLocation();
	}
	else
	{
		Home = GYM_HOME;
		// 무기 · 회복 발판을 끈다 — 걷는 길에서 무기를 주워 장비가 바뀌지 않게(몸 맞힘 촬영과 같은 처리)
		for (TActorIterator<ALyraWeaponSpawner> It(World); It; ++It)
		{
			It->SetActorHiddenInGame(true);
			It->SetActorEnableCollision(false);
			It->SetActorTickEnabled(false);
		}
		// 상대(봇)는 처음부터 세워 둔다 — 조준 · 앉기 칸 동안 나를 쏘거나 길을 막지 않게(몸 맞힘 촬영과 같은 처리)
		for (TActorIterator<ACharacter> It(World); It; ++It)
		{
			if (*It != Ch && FindASC(*It) && It->GetController()) { Foe = *It; break; }
		}
		if (AAIController* AI = Foe.IsValid() ? Cast<AAIController>(Foe->GetController()) : nullptr)
		{
			if (AI->BrainComponent) AI->BrainComponent->StopLogic(TEXT("MOBContentProbe"));
			AI->StopMovement();
			AI->ClearFocus(EAIFocusPriority::Gameplay);
			AI->bSetControlRotationFromPawnOrientation = false;
		}
		GoHome();
		PlaceFoe(-400.0, true);
	}

	Csv += TEXT("frame,t,step,ev,px,py,pz,vx,vy,vz,speed2d,maxSpeed,crouched,capHH,eyeH,camW,camAds,fov,montage,montagePos,meleeActive,adsActive,")
		TEXT("foeX,foeY,foeYaw,foeHp,foeMontage,foeMontagePos,hp,padBigAvail,padBigPct,padSmallAvail,padSmallPct,")
		TEXT("handX,handY,handZ,yaw,foeZ,foeHH,walk,mode,foeWalk,foeVx,foeVy\n");
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_READY pawn=%s home=%s steps=%d"), *GetNameSafe(Ch), *Home.ToString(), StepCount(ScriptIdx()));
	return true;
}

void UMOBContentProbeSubsystem::GoHome() const
{
	ACharacter* Ch = Me();
	if (!Ch) return;
	Ch->SetActorLocationAndRotation(Home, FRotator::ZeroRotator, false, nullptr, ETeleportType::TeleportPhysics);
	if (UCharacterMovementComponent* M = Ch->GetCharacterMovement()) M->StopMovementImmediately();
	if (APlayerController* PC = Cast<APlayerController>(Ch->GetController())) PC->SetControlRotation(FRotator::ZeroRotator);
}

void UMOBContentProbeSubsystem::PlaceFoe(double Distance, bool bFacingMe, double Side) const
{
	ACharacter* F = Foe.Get();
	const ACharacter* Ch = Me();
	if (!F || !Ch) return;
	const FRotator Rot(0.0, bFacingMe ? 180.0 : 0.0, 0.0);
	F->SetActorLocationAndRotation(Ch->GetActorLocation() + FVector(Distance, Side, 0.0), Rot, false, nullptr, ETeleportType::TeleportPhysics);
	if (UCharacterMovementComponent* M = F->GetCharacterMovement()) M->StopMovementImmediately();
	if (AController* C = F->GetController()) C->SetControlRotation(Rot);
}

// 점수판 칸 그대로 — Lyra 점수판 줄(W_SB_PlayerRow_TDM)이 읽는 선수 상태의 통계 표식 셋
void UMOBContentProbeSubsystem::LogScores(const TCHAR* When) const
{
	static const FGameplayTag Elim = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.Score.Eliminations"));
	static const FGameplayTag Death = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.Score.Deaths"));
	static const FGameplayTag Assist = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.Score.Assists"));
	const AGameStateBase* GS = GetWorld()->GetGameState();
	if (!GS) return;
	FString Line;
	int32 SumAssist = 0;
	for (const APlayerState* PS : GS->PlayerArray)
	{
		const ALyraPlayerState* L = Cast<ALyraPlayerState>(PS);
		if (!L) continue;
		const int32 A = L->GetStatTagStackCount(Assist);
		SumAssist += A;
		Line += FString::Printf(TEXT("%s:%d/%d/%d "), *L->GetPlayerName(), L->GetStatTagStackCount(Elim), L->GetStatTagStackCount(Death), A);
	}
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_SCORE when=%s t=%.2f players=%d sumAssist=%d [name:elim/death/assist] %s"), When, GetWorld()->GetTimeSeconds(), GS->PlayerArray.Num(), SumAssist, *Line.TrimEnd());
}

void UMOBContentProbeSubsystem::PressTag(const TCHAR* TagName, bool bPress) const
{
	ULyraAbilitySystemComponent* A = Cast<ULyraAbilitySystemComponent>(FindASC(Me()));
	if (!A) return;
	const FGameplayTag T = FGameplayTag::RequestGameplayTag(TagName);
	if (bPress) A->AbilityInputTagPressed(T);
	else A->AbilityInputTagReleased(T);
}

bool UMOBContentProbeSubsystem::IsAbilityActive(const TCHAR* ClassNamePart) const
{
	const UAbilitySystemComponent* A = FindASC(Me());
	if (!A) return false;
	for (const FGameplayAbilitySpec& Spec : A->GetActivatableAbilities())
	{
		if (Spec.Ability && Spec.IsActive() && Spec.Ability->GetClass()->GetName().Contains(ClassNamePart)) return true;
	}
	return false;
}

float UMOBContentProbeSubsystem::HealthOf(const AActor* A) const
{
	const UAbilitySystemComponent* ASC = FindASC(A);
	return ASC ? ASC->GetNumericAttribute(ULyraHealthSet::GetHealthAttribute()) : -1.f;
}

void UMOBContentProbeSubsystem::SetHealthOf(AActor* A, float Max, float Value) const
{
	if (UAbilitySystemComponent* ASC = FindASC(A))
	{
		ASC->SetNumericAttributeBase(ULyraHealthSet::GetMaxHealthAttribute(), Max);
		ASC->SetNumericAttributeBase(ULyraHealthSet::GetHealthAttribute(), Value);
	}
}

ALyraWeaponSpawner* UMOBContentProbeSubsystem::PadNear(const FVector& Where) const
{
	ALyraWeaponSpawner* Best = nullptr;
	double BestD = 0;
	for (TActorIterator<ALyraWeaponSpawner> It(GetWorld()); It; ++It)
	{
		const double D = FVector::Dist(It->GetActorLocation(), Where);
		if (!Best || D < BestD) { Best = *It; BestD = D; }
	}
	return BestD < 50.0 ? Best : nullptr;
}

// 받침 위 바닥을 찾아 그 위에 선다(캡슐 바닥이 바닥에서 3cm 위 — 이동 기록 도구의 START_ABOVE_FLOOR 와 같다)
void UMOBContentProbeSubsystem::StandOn(const FVector& Where) const
{
	ACharacter* Ch = Me();
	if (!Ch) return;
	UCapsuleComponent* Cap = Ch->GetCapsuleComponent();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(MOBCProbePad), false, Ch);
	const FCollisionResponseParams Response(Cap->GetCollisionResponseToChannels());
	FHitResult Hit;
	FVector Spot = Where + FVector(0, 0, Cap->GetScaledCapsuleHalfHeight());
	if (GetWorld()->LineTraceSingleByChannel(Hit, Where + FVector(0, 0, 200), Where - FVector(0, 0, 600), Cap->GetCollisionObjectType(), Params, Response))
	{
		Spot = Hit.ImpactPoint + FVector(0, 0, Cap->GetScaledCapsuleHalfHeight() + 3.0);
	}
	Ch->SetActorLocation(Spot, false, nullptr, ETeleportType::TeleportPhysics);
	if (UCharacterMovementComponent* M = Ch->GetCharacterMovement()) M->StopMovementImmediately();
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_STAND pad=%s floorHit=%d spot=%s"), *Where.ToString(), Hit.bBlockingHit ? 1 : 0, *Spot.ToString());
}

bool UMOBContentProbeSubsystem::PadAvailable(const ALyraWeaponSpawner* Pad, float& OutPercent)
{
	OutPercent = -1.f;
	if (!Pad) return false;
	if (const FFloatProperty* F = CastField<FFloatProperty>(Pad->GetClass()->FindPropertyByName(TEXT("CoolDownPercentage")))) OutPercent = F->GetPropertyValue_InContainer(Pad);
	const FBoolProperty* B = CastField<FBoolProperty>(Pad->GetClass()->FindPropertyByName(TEXT("bIsWeaponAvailable")));
	return B && B->GetPropertyValue_InContainer(Pad);
}

void UMOBContentProbeSubsystem::BeginStep()
{
	const FStep& S = StepAt(ScriptIdx(), Step);
	ACharacter* Ch = Me();
	FrameEvents += FString::Printf(TEXT("begin_%s;"), S.Name);
	bMeleePressed = false;
	MeleePressFrame = -1;
	bJumpPending = false;
	JumpPressFrame = -1;
	// 근접 한 번 칸은 순간이동으로 시작한다 — 앞 칸 마지막 프레임에 넣은 이동 입력이 이 칸 첫 이동에 먹지 않게 비운다(17-d d3q 점프 칸 첫 줄 vx=20).
	// 걷기 · 조준 칸은 이어 걷는 것을 재므로 그대로 둔다
	if (Ch && IsMeleeOnce(S.Act)) Ch->ConsumeMovementInputVector();
	// 부딪힘 칸도 순간이동으로 시작한다 — 나 · 상대 모두 앞 칸 입력을 비운다(F-1)
	if (IsBump(S.Act))
	{
		if (Ch) Ch->ConsumeMovementInputVector();
		if (ACharacter* F = Foe.Get()) F->ConsumeMovementInputVector();
	}
	FoeHitFrame = -1;
	LungeMax = 0;
	MeleeActiveFrames = 0;
	FoeMontageSeen.Reset();
	MyMontageSeen.Reset();
	MeleeStarts.Reset();
	bMeleeWasActive = IsAbilityActive(TEXT("GA_Melee"));
	MeAtStepBegin = Ch ? Ch->GetActorLocation() : FVector::ZeroVector;
	NextWatchLogAt = 0.f;

	switch (S.Act)
	{
	case EAct::None:
		break;
	case EAct::Home:
		GoHome();
		break;
	case EAct::AdsPress:
		PressTag(TEXT("InputTag.Weapon.ADS"), true);
		break;
	case EAct::AdsRelease:
		PressTag(TEXT("InputTag.Weapon.ADS"), false);
		break;
	case EAct::CrouchToggle:
		// 키 입력과 같은 길 — Lyra 의 앉기 키는 능력이 아니라 영웅 컴포넌트가 이 함수를 부른다(LyraCharacter.cpp:426)
		if (ALyraCharacter* L = Cast<ALyraCharacter>(Ch)) L->ToggleCrouch();
		break;
	case EAct::Equip:
	{
		GoHome();
		UClass* Def = nullptr;
		const bool bOk = EquipWeapon(Ch, (int32)S.Arg, Def);
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_EQUIP step=%s def=%s ok=%d"), S.Name, *GetNameSafe(Def), bOk ? 1 : 0);
		break;
	}
	case EAct::FoeSetup:
	{
		ACharacter* F = Foe.Get();
		if (!F)
		{
			UE_LOG(LogMOBCProbe, Error, TEXT("MOB_CPROBE_FAIL noFoe"));
			break;
		}
		// 다른 팀이어야 근접이 피해를 준다(GA_Melee 의 CompareTeams → DifferentTeams 갈래)
		ULyraTeamSubsystem* TS = GetWorld()->GetSubsystem<ULyraTeamSubsystem>();
		int32 TeamMe = -1, TeamFoe = -1;
		ELyraTeamComparison Cmp = TS ? TS->CompareTeams(Ch, F, TeamMe, TeamFoe) : ELyraTeamComparison::InvalidArgument;
		const int32 Before = (int32)Cmp;
		if (TS && Cmp != ELyraTeamComparison::DifferentTeams)
		{
			TS->ChangeTeamForActor(F, TeamMe == 1 ? 2 : 1);
			Cmp = TS->CompareTeams(Ch, F, TeamMe, TeamFoe);
		}
		// 한 방에 죽지 않게 체력을 크게 — 피해 값만 본다
		SetHealthOf(F, 1000.f, 1000.f);
		static const FGameplayTag Immune = FGameplayTag::RequestGameplayTag(TEXT("Gameplay.DamageImmunity"));
		const UAbilitySystemComponent* FoeASC = FindASC(F);
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_FOE foe=%s teamMe=%d teamFoe=%d cmpBefore=%d cmpAfter=%d foeHp=%.1f foeImmune=%d"), *GetNameSafe(F), TeamMe, TeamFoe, Before, (int32)Cmp, HealthOf(F),
			FoeASC && FoeASC->HasMatchingGameplayTag(Immune) ? 1 : 0);
		GoHome();
		PlaceFoe(-400.0, true);
		break;
	}
	case EAct::FoeSameTeam:
	{
		ACharacter* F = Foe.Get();
		ULyraTeamSubsystem* TS = GetWorld()->GetSubsystem<ULyraTeamSubsystem>();
		int32 TeamMe = -1, TeamFoe = -1;
		if (F && TS)
		{
			TS->CompareTeams(Ch, F, TeamMe, TeamFoe);
			TS->ChangeTeamForActor(F, TeamMe);
		}
		const ELyraTeamComparison Cmp = (F && TS) ? TS->CompareTeams(Ch, F, TeamMe, TeamFoe) : ELyraTeamComparison::InvalidArgument;
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_FOE_SAME foe=%s teamMe=%d teamFoe=%d cmp=%d"), *GetNameSafe(F), TeamMe, TeamFoe, (int32)Cmp);
		GoHome();
		PlaceFoe(-400.0, true);
		break;
	}
	case EAct::MeleeAt:
	case EAct::MeleeAtBack:
		GoHome();
		PlaceFoe(S.Arg, S.Act == EAct::MeleeAt);
		break;
	case EAct::MeleeAtSide:
		GoHome();
		PlaceFoe(MELEE_SIDE_FORWARD, true, S.Arg);
		break;
	case EAct::MeleeHere:
		break;
	case EAct::BumpStillAt:
	case EAct::BumpAt:
		GoHome();
		PlaceFoe(S.Arg, true);
		break;
	case EAct::BumpStillSideAt:
	case EAct::BumpSideAt:
		GoHome();
		PlaceFoe(BUMP_FORWARD, true, S.Arg);
		break;
	case EAct::FoeCrouchToggle:
		// 내 앉기와 같은 길(영웅 컴포넌트가 부르는 ToggleCrouch)
		if (ALyraCharacter* L = Cast<ALyraCharacter>(Foe.Get())) L->ToggleCrouch();
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_FOE_CROUCH step=%s foe=%s"), S.Name, *GetNameSafe(Foe.Get()));
		break;
	case EAct::MeleeJumpAt:
		GoHome();
		PlaceFoe(S.Arg, true);
		bJumpPending = true;		// 누르기는 칸 첫 프레임 입력에서(DuringStep)
		break;
	case EAct::Watch:
		LogScores(TEXT("begin"));
		break;
	case EAct::MeleeSpam:
		GoHome();
		PlaceFoe(-400.0, true);	// 등 뒤 — 쓸기에 안 걸리게(밀기 없이 간격만)
		break;
	case EAct::PadsDump:
		for (TActorIterator<ALyraWeaponSpawner> It(GetWorld()); It; ++It)
		{
			const UCapsuleComponent* Cap = It->FindComponentByClass<UCapsuleComponent>();
			float Pct = 0.f;
			const bool bAvail = PadAvailable(*It, Pct);
			UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_PADINFO name=%s class=%s loc=%s avail=%d coolDownTime=%s def=%s effect=%s capsuleR=%.1f capsuleHH=%.1f"),
				*It->GetName(), *It->GetClass()->GetName(), *It->GetActorLocation().ToString(), bAvail ? 1 : 0, *PropText(*It, TEXT("CoolDownTime")),
				*PropText(*It, TEXT("WeaponDefinition")), *PropText(*It, TEXT("GameplayEffectToApply")),
				Cap ? Cap->GetScaledCapsuleRadius() : -1.f, Cap ? Cap->GetScaledCapsuleHalfHeight() : -1.f);
		}
		break;
	case EAct::PadOn:
		StandOn(PAD_SPOTS[FMath::Clamp((int32)S.Arg, 0, 1)]);
		break;
	case EAct::Away:
		GoHome();
		break;
	case EAct::SetHealth:
		if (const UAbilitySystemComponent* ASC = FindASC(Ch)) SetHealthOf(Ch, ASC->GetNumericAttribute(ULyraHealthSet::GetMaxHealthAttribute()), S.Arg);
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_SETHP hp=%.1f"), HealthOf(Ch));
		break;
	}
}

void UMOBContentProbeSubsystem::DuringStep()
{
	const FStep& S = StepAt(ScriptIdx(), Step);
	ACharacter* Ch = Me();
	const double Now = GetWorld()->GetTimeSeconds();
	if (S.bWalk && Ch) Ch->AddMovementInput(FVector::ForwardVector, 1.f);
	if (FoeWalks(S.Act))
	{
		if (ACharacter* F = Foe.Get()) F->AddMovementInput(-FVector::ForwardVector, 1.f);
	}
	// 뛰며 근접 — 이동 기록 · 다리와 같은 길(ACharacter::Jump). 이 프레임에 누르면 다음 프레임 이동이 뛰고, 그다음 프레임에 뗀다
	if (bJumpPending && Ch)
	{
		Ch->Jump();
		bJumpPending = false;
		JumpPressFrame = Frame;
		FrameEvents += TEXT("jump_press;");
	}
	else if (JumpPressFrame >= 0 && Frame > JumpPressFrame && Ch)
	{
		Ch->StopJumping();
		JumpPressFrame = -1;
		FrameEvents += TEXT("jump_release;");
	}

	const bool bMeleeActive = IsAbilityActive(TEXT("GA_Melee"));
	if (bMeleeActive && !bMeleeWasActive)
	{
		MeleeStarts.Add(Now);
		FrameEvents += TEXT("melee_start;");
		// E-1 시험판 — 능력과 같은 쓸기를 앞 프레임 손 · 이 프레임 손으로 해 본다(어느 손인지는 밀기 속도 = 2200 × 맞은 거리 ÷ 350 으로 가린다)
		if (Ch && bPrevHandValid) LogMeleeTrace(TEXT("prev"), PrevHand);
		if (Ch) LogMeleeTrace(TEXT("now"), HandOf(Ch));
		// 상대 몸체 — 앞 프레임(능력이 본 것으로 짐작, side_p130 에서 내 재시험만 메시에 닿음) · 이 프레임(내 재시험이 본 것)
		if (const ACharacter* FNow = Foe.Get())
		{
			if (PrevFoeParts.Num() > 0) LogFoeParts(TEXT("prev"), PrevFoeParts, PrevFoePartNames, PrevFoeLoc, PrevFoeYaw);
			TArray<FTransform> NowParts;
			TArray<FName> NowNames;
			CollectParts(FNow, NowParts, NowNames);
			LogFoeParts(TEXT("now"), NowParts, NowNames, FNow->GetActorLocation(), FNow->GetActorRotation().Yaw);
		}
	}
	if (!bMeleeActive && bMeleeWasActive) FrameEvents += TEXT("melee_end;");
	if (bMeleeActive) ++MeleeActiveFrames;
	bMeleeWasActive = bMeleeActive;

	if (IsMeleeOnce(S.Act))
	{
		if (!bMeleePressed && StepTime >= MELEE_PRESS_AT && Ch)
		{
			FoeHpAtPress = HealthOf(Foe.Get());
			FoeHpMin = FoeHpAtPress;
			MeAtPress = Ch->GetActorLocation();
			PressTag(TEXT("InputTag.Ability.Melee"), true);
			bMeleePressed = true;
			MeleePressFrame = Frame;
			FrameEvents += TEXT("melee_press;");
		}
		else if (bMeleePressed && Frame == MeleePressFrame + 1)
		{
			PressTag(TEXT("InputTag.Ability.Melee"), false);
			FrameEvents += TEXT("melee_release;");
		}
		if (bMeleePressed && Ch)
		{
			const float Hp = HealthOf(Foe.Get());
			if (Hp < FoeHpMin)
			{
				if (FoeHitFrame < 0) FoeHitFrame = Frame;
				FoeHpMin = Hp;
			}
			LungeMax = FMath::Max(LungeMax, (Ch->GetActorLocation() - MeAtPress).Size2D());
			const FString FM = MontageOf(Foe.Get());
			if (!FM.IsEmpty() && !FoeMontageSeen.Contains(FM)) FoeMontageSeen += FM + TEXT(" ");
			const FString MM = MontageOf(Ch);
			if (!MM.IsEmpty() && !MyMontageSeen.Contains(MM)) MyMontageSeen += MM + TEXT(" ");
		}
	}
	else if (S.Act == EAct::MeleeSpam)
	{
		PressTag(TEXT("InputTag.Ability.Melee"), true);
	}
	else if (S.Act == EAct::Watch && StepTime >= NextWatchLogAt)
	{
		LogScores(TEXT("watch"));
		NextWatchLogAt += WATCH_LOG_EVERY;
	}

	if (bPads && Ch)
	{
		const float Hp = HealthOf(Ch);
		if (Hp != LastHp)
		{
			UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_HP t=%.4f step=%s stepT=%.4f from=%.1f to=%.1f"), Now, S.Name, StepTime, LastHp, Hp);
			LastHp = Hp;
		}
		for (int32 i = 0; i < 2; ++i)
		{
			float Pct = 0.f;
			const int32 Avail = PadAvailable(PadNear(PAD_SPOTS[i]), Pct) ? 1 : 0;
			if (Avail != LastPadAvail[i])
			{
				UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_PAD t=%.4f step=%s stepT=%.4f pad=%s avail=%d pct=%.3f"), Now, S.Name, StepTime, i == 0 ? TEXT("big") : TEXT("small"), Avail, Pct);
				LastPadAvail[i] = Avail;
			}
		}
	}
}

void UMOBContentProbeSubsystem::EndStep()
{
	const FStep& S = StepAt(ScriptIdx(), Step);
	ACharacter* Ch = Me();
	if (S.Act == EAct::Equip) LogBodies(S.Name);		// 쥔 뒤 칸 끝 — 무기 층이 다시 붙은 뒤다
	if (IsMeleeOnce(S.Act))
	{
		const ACharacter* F = Foe.Get();
		const FVector Rel = (F && Ch) ? F->GetActorLocation() - MeAtPress : FVector::ZeroVector;
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_MELEE step=%s arg=%.0f foeRelX=%.1f foeRelY=%.1f facingMe=%d pressed=%d hit=%d dmg=%.2f hitFrames=%d lunge2d=%.2f activeSec=%.4f myMontage=[%s] foeMontage=[%s]"),
			S.Name, S.Arg, Rel.X, Rel.Y, S.Act != EAct::MeleeAtBack ? 1 : 0, bMeleePressed ? 1 : 0, FoeHitFrame >= 0 ? 1 : 0, FoeHpAtPress - FoeHpMin,
			FoeHitFrame >= 0 ? FoeHitFrame - MeleePressFrame : -1, LungeMax, MeleeActiveFrames / 60.0, *MyMontageSeen.TrimEnd(), *FoeMontageSeen.TrimEnd());
		if (!bBridge) SetHealthOf(Foe.Get(), 1000.f, 1000.f);
	}
	else if (S.Act == EAct::Watch)
	{
		LogScores(TEXT("end"));
	}
	else if (S.Act == EAct::MeleeSpam)
	{
		PressTag(TEXT("InputTag.Ability.Melee"), false);
		FString Gaps;
		for (int32 i = 1; i < MeleeStarts.Num(); ++i) Gaps += FString::Printf(TEXT("%.4f "), MeleeStarts[i] - MeleeStarts[i - 1]);
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_SPAM step=%s starts=%d gaps=[%s] montage=%s"), S.Name, MeleeStarts.Num(), *Gaps.TrimEnd(), *MontageOf(Ch));
	}
	else if (Ch)
	{
		const UCharacterMovementComponent* M = Ch->GetCharacterMovement();
		float W = -1.f;
		bool bAds = false;
		ReadAimCamera(Ch, W, bAds);
		const APlayerController* PC = Cast<APlayerController>(Ch->GetController());
		const FVector Moved = Ch->GetActorLocation() - MeAtStepBegin;
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_STEP step=%s speed2d=%.2f maxSpeed=%.2f crouched=%d capHH=%.2f eyeH=%.2f camW=%.4f camAds=%d fov=%.3f ads=%d moved2d=%.2f movedZ=%.2f"),
			S.Name, M ? M->Velocity.Size2D() : -1.f, M ? M->GetMaxSpeed() : -1.f, Ch->bIsCrouched ? 1 : 0, Ch->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(),
			Ch->BaseEyeHeight, W, bAds ? 1 : 0, PC && PC->PlayerCameraManager ? PC->PlayerCameraManager->GetFOVAngle() : -1.f, IsAbilityActive(TEXT("GA_ADS")) ? 1 : 0,
			Moved.Size2D(), Moved.Z);
	}
}

void UMOBContentProbeSubsystem::WriteRow()
{
	ACharacter* Ch = Me();
	if (!Ch) return;
	const UCharacterMovementComponent* M = Ch->GetCharacterMovement();
	const FVector P = Ch->GetActorLocation();
	const FVector V = M ? M->Velocity : FVector::ZeroVector;
	float W = -1.f;
	bool bAds = false;
	ReadAimCamera(Ch, W, bAds);
	const APlayerController* PC = Cast<APlayerController>(Ch->GetController());
	float MPos = -1.f;
	const FString MName = MontageOf(Ch, &MPos);
	const ACharacter* F = Foe.Get();

	Csv += FString::Printf(TEXT("%d,%.4f,%s,%s,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.3f,%.3f,%d,%.3f,%.3f,%.5f,%d,%.4f,%s,%.4f,%d,%d,"),
		Frame, GetWorld()->GetTimeSeconds(), StepAt(ScriptIdx(), Step).Name, *FrameEvents, P.X, P.Y, P.Z, V.X, V.Y, V.Z, V.Size2D(), M ? M->GetMaxSpeed() : -1.f,
		Ch->bIsCrouched ? 1 : 0, Ch->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), Ch->BaseEyeHeight, W, bAds ? 1 : 0,
		PC && PC->PlayerCameraManager ? PC->PlayerCameraManager->GetFOVAngle() : -1.f, *MName, MPos,
		IsAbilityActive(TEXT("GA_Melee")) ? 1 : 0, IsAbilityActive(TEXT("GA_ADS")) ? 1 : 0);
	float FoeMPos = -1.f;
	const FString FoeMName = MontageOf(F, &FoeMPos);
	Csv += FString::Printf(TEXT("%.5f,%.5f,%.2f,%.2f,%s,%.4f,%.2f,"),
		F ? F->GetActorLocation().X : 0.0, F ? F->GetActorLocation().Y : 0.0, F ? F->GetActorRotation().Yaw : 0.0, HealthOf(F), *FoeMName, FoeMPos, HealthOf(Ch));
	float PctBig = -1.f, PctSmall = -1.f;
	int32 AvailBig = -1, AvailSmall = -1;
	if (bPads)
	{
		AvailBig = PadAvailable(PadNear(PAD_SPOTS[0]), PctBig) ? 1 : 0;
		AvailSmall = PadAvailable(PadNear(PAD_SPOTS[1]), PctSmall) ? 1 : 0;
	}
	Csv += FString::Printf(TEXT("%d,%.4f,%d,%.4f,"), AvailBig, PctBig, AvailSmall, PctSmall);
	const FVector Hand = HandOf(Ch);
	// walk = 이 프레임에 +X 누름을 넣었나(각본 칸의 bWalk), mode = 이동 모드(1 걷기 · 3 낙하) — 밀기 재생이 누름 · 낙하로 시작하려고(17-d)
	// foeWalk = 이 프레임에 상대에게 -X 누름을 넣었나, foeVx · foeVy = 상대 속도(F-1 부딪힘)
	const UCharacterMovementComponent* FM = F ? F->GetCharacterMovement() : nullptr;
	const FVector FV = FM ? FM->Velocity : FVector::ZeroVector;
	Csv += FString::Printf(TEXT("%.3f,%.3f,%.3f,%.4f,%.5f,%.3f,%d,%d,%d,%.5f,%.5f\n"), Hand.X, Hand.Y, Hand.Z, Ch->GetActorRotation().Yaw,
		F ? F->GetActorLocation().Z : 0.0, F ? F->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 0.f,
		StepAt(ScriptIdx(), Step).bWalk ? 1 : 0, M ? (int32)M->MovementMode.GetValue() : -1,
		FoeWalks(StepAt(ScriptIdx(), Step).Act) ? 1 : 0, FV.X, FV.Y);
	PrevHand = Hand;
	bPrevHandValid = true;
	CollectParts(F, PrevFoeParts, PrevFoePartNames);
	PrevFoeLoc = F ? F->GetActorLocation() : FVector::ZeroVector;
	PrevFoeYaw = F ? F->GetActorRotation().Yaw : 0.0;
	FrameEvents.Reset();
}

// E-1 시험판 — GA_Melee 의 쓸기를 그대로 해 본다(시작 hand_r · 몸 앞 × 350 · 반지름 96 · 반높이 48 · 대상 종류 Pawn · 나 빼고, 0-3 표).
// 단일 쓸기 한 번(능력과 같음) + 다중 쓸기 한 번(상대 캡슐 · 메시 중 무엇이 먼저 닿나). 맞았으면 벽 광선(무기 채널, 맞은 액터 · 나 빼고)도 같은 식으로
void UMOBContentProbeSubsystem::LogMeleeTrace(const TCHAR* Which, const FVector& Hand) const
{
	ACharacter* Ch = Me();
	if (!Ch) return;
	const FVector Fwd = Ch->GetActorForwardVector();
	const FVector End = Hand + Fwd * MELEE_TRACE_DISTANCE;
	TArray<TEnumAsByte<EObjectTypeQuery>> Types;
	Types.Add(UEngineTypes::ConvertToObjectType(ECC_Pawn));
	const TArray<AActor*> Ignore = { Ch };
	FHitResult Hit;
	const bool bHit = UKismetSystemLibrary::CapsuleTraceSingleForObjects(Ch, Hand, End, MELEE_TRACE_RADIUS, MELEE_TRACE_HALF_HEIGHT, Types, false, Ignore, EDrawDebugTrace::None, Hit, true);
	int32 Wall = -1;
	if (bHit)
	{
		const TArray<AActor*> Ignore2 = { Hit.GetActor(), Ch };
		FHitResult WallHit;
		Wall = UKismetSystemLibrary::LineTraceSingle(Ch, Hand, Hit.ImpactPoint, UEngineTypes::ConvertToTraceType(Lyra_TraceChannel_Weapon), false, Ignore2, EDrawDebugTrace::None, WallHit, true) ? 1 : 0;
	}
	const ACharacter* F = Foe.Get();
	const FVector FL = F ? F->GetActorLocation() : FVector::ZeroVector;
	const FVector C = Ch->GetActorLocation();
	const USkeletalMesh* SM = Ch->GetMesh() ? Ch->GetMesh()->GetSkeletalMeshAsset() : nullptr;
	const int32 MeshSocket = SM && SM->FindSocket(TEXT("hand_r")) ? 1 : 0;
	const double BoneDiff = Ch->GetMesh() ? FVector::Dist(HandOf(Ch), Ch->GetMesh()->GetBoneLocation(TEXT("hand_r"))) : -1.0;
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_MTRACE step=%s which=%s frame=%d meshSocket=%d boneDiff=%.5f hand=%.4f,%.4f,%.4f fwd=%.6f,%.6f,%.6f center=%.4f,%.4f,%.4f crouched=%d hit=%d actor=%s comp=%s dist=%.4f time=%.6f loc=%.4f,%.4f,%.4f impact=%.4f,%.4f,%.4f normal=%.5f,%.5f,%.5f impactNormal=%.5f,%.5f,%.5f startPen=%d pen=%.4f wall=%d foe=%.4f,%.4f,%.4f foeYaw=%.3f foeR=%.3f foeHH=%.3f"),
		StepAt(ScriptIdx(), Step).Name, Which, Frame, MeshSocket, BoneDiff, Hand.X, Hand.Y, Hand.Z, Fwd.X, Fwd.Y, Fwd.Z, C.X, C.Y, C.Z, Ch->bIsCrouched ? 1 : 0, bHit ? 1 : 0,
		*GetNameSafe(Hit.GetActor()), *GetNameSafe(Hit.GetComponent()), Hit.Distance, Hit.Time, Hit.Location.X, Hit.Location.Y, Hit.Location.Z,
		Hit.ImpactPoint.X, Hit.ImpactPoint.Y, Hit.ImpactPoint.Z, Hit.Normal.X, Hit.Normal.Y, Hit.Normal.Z, Hit.ImpactNormal.X, Hit.ImpactNormal.Y, Hit.ImpactNormal.Z,
		Hit.bStartPenetrating ? 1 : 0, Hit.PenetrationDepth, Wall, FL.X, FL.Y, FL.Z, F ? F->GetActorRotation().Yaw : 0.0,
		F ? F->GetCapsuleComponent()->GetScaledCapsuleRadius() : 0.f, F ? F->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 0.f);
	TArray<FHitResult> Hits;
	UKismetSystemLibrary::CapsuleTraceMultiForObjects(Ch, Hand, End, MELEE_TRACE_RADIUS, MELEE_TRACE_HALF_HEIGHT, Types, false, Ignore, EDrawDebugTrace::None, Hits, true);
	FString List;
	for (const FHitResult& H : Hits) List += FString::Printf(TEXT("%s:%s:%.4f:%d "), *GetNameSafe(H.GetActor()), *GetNameSafe(H.GetComponent()), H.Distance, H.bStartPenetrating ? 1 : 0);
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_MTRACE_ALL step=%s which=%s n=%d [actor:comp:dist:startPen] %s"), StepAt(ScriptIdx(), Step).Name, Which, Hits.Num(), *List.TrimEnd());
}

// 상대 몸체를 몸 기준(캡슐 중심, 요 0)으로 — 다리 MOB_VIEWP 와 같은 꼴. 서버 근접 판정이 같은 몸으로 같은 결과를 내나 대조한다(Tools/MeleeCheck)
void UMOBContentProbeSubsystem::LogFoeParts(const TCHAR* Which, const TArray<FTransform>& Parts, const TArray<FName>& Names, const FVector& Loc, double Yaw) const
{
	const FQuat ActorInv = FRotator(0, Yaw, 0).Quaternion().Inverse();
	for (int32 i = 0; i < Parts.Num(); ++i)
	{
		const FTransform& W = Parts[i];
		const FVector P = ActorInv.RotateVector(W.GetLocation() - Loc);
		const FVector X = ActorInv.RotateVector(W.GetUnitAxis(EAxis::X));
		const FVector Y = ActorInv.RotateVector(W.GetUnitAxis(EAxis::Y));
		const FVector Z = ActorInv.RotateVector(W.GetUnitAxis(EAxis::Z));
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_MPART step=%s which=%s at=%.4f,%.4f,%.4f yaw=%.4f bone=%s p=%.4f,%.4f,%.4f x=%.5f,%.5f,%.5f y=%.5f,%.5f,%.5f z=%.5f,%.5f,%.5f"),
			StepAt(ScriptIdx(), Step).Name, Which, Loc.X, Loc.Y, Loc.Z, Yaw, *Names[i].ToString(), P.X, P.Y, P.Z, X.X, X.Y, X.Z, Y.X, Y.Y, Y.Z, Z.X, Z.Y, Z.Z);
	}
}

bool UMOBContentProbeSubsystem::EquipWeapon(ACharacter* C, int32 Weapon, UClass*& OutDef) const
{
	ULyraEquipmentManagerComponent* EM = C ? C->FindComponentByClass<ULyraEquipmentManagerComponent>() : nullptr;
	OutDef = LoadClass<ULyraEquipmentDefinition>(nullptr, WEAPON_DEFS[FMath::Clamp(Weapon, 0, 2)]);
	if (!EM || !OutDef) return false;
	const TArray<ULyraEquipmentInstance*> Old = EM->GetEquipmentInstancesOfType(ULyraEquipmentInstance::StaticClass());
	for (ULyraEquipmentInstance* I : Old) EM->UnequipItem(I);
	EM->EquipItem(OutDef);
	return true;
}

// 다리의 남의 몸 맞추기(MOBridgeSubsystem::EnsureRemoteBody)와 같은 길 — Lyra 가 몸을 고르는 시점이 늦을 수 있어 맞을 때까지 0.5초마다 다시 건다
bool UMOBContentProbeSubsystem::QuinnReady(float DeltaTime)
{
	static const FName QUINN_BODY(TEXT("SKM_Quinn_Invis"));
	constexpr int32 BODY_TRY_LIMIT = 20;
	constexpr double BODY_RETRY_SECONDS = 0.5;
	constexpr double BODY_SETTLE_SECONDS = 2.0;		// 다시 쥐는 몽타주(권총 쥐기 약 1초)가 끝나고 대기 자세로 돌아올 만큼
	const auto IsQuinn = [](const ACharacter* C)
	{
		const USkeletalMeshComponent* M = C ? C->GetMesh() : nullptr;
		const USkeletalMesh* A = M ? M->GetSkeletalMeshAsset() : nullptr;
		return A && A->GetFName() == QUINN_BODY;
	};
	BodySeconds += DeltaTime;
	if (BodyEquippedAt >= 0.0) return BodySeconds - BodyEquippedAt >= BODY_SETTLE_SECONDS;
	ACharacter* Ch = Me();
	ACharacter* F = Foe.Get();
	const bool bMe = IsQuinn(Ch);
	const bool bFoe = !F || IsQuinn(F);
	if (bMe && bFoe)
	{
		LogBodies(TEXT("quinn"));
		UClass* Def = nullptr;
		const bool bOkMe = EquipWeapon(Ch, 0, Def);
		const bool bOkFoe = !F || EquipWeapon(F, 0, Def);
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_EQUIP step=quinn def=%s ok=%d foeOk=%d"), *GetNameSafe(Def), bOkMe ? 1 : 0, bOkFoe ? 1 : 0);
		GoHome();
		if (!bBridge && !bPads) PlaceFoe(-400.0, true);
		BodyEquippedAt = BodySeconds;
		return false;
	}
	if (BodySeconds < NextBodyTryAt) return false;
	if (BodyTries >= BODY_TRY_LIMIT)
	{
		UE_LOG(LogMOBCProbe, Error, TEXT("MOB_CPROBE_FAIL body me=%d foe=%d tries=%d"), bMe ? 1 : 0, bFoe ? 1 : 0, BodyTries);
		Finish(false);
		return false;
	}
	NextBodyTryAt = BodySeconds + BODY_RETRY_SECONDS;
	++BodyTries;
	if (!bMe) MOBSetBodyPart(Ch, TEXT("/Game/Characters/Cosmetics/B_Quinn.B_Quinn_C"));
	if (!bFoe) MOBSetBodyPart(F, TEXT("/Game/Characters/Cosmetics/B_Quinn.B_Quinn_C"));
	return false;
}

void UMOBContentProbeSubsystem::LogBodies(const TCHAR* When) const
{
	const auto Describe = [](const ACharacter* C)
	{
		const USkeletalMeshComponent* M = C ? C->GetMesh() : nullptr;
		FString Out = GetNameSafe(M ? M->GetSkeletalMeshAsset() : nullptr);
		if (M)
		{
			for (const UAnimInstance* L : M->GetLinkedAnimInstances()) Out += TEXT(" ") + GetNameSafe(L ? L->GetClass() : nullptr);
		}
		return Out;
	};
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_BODY when=%s forceQuinn=%d tries=%d me=[%s] foe=[%s]"), When, bForceQuinn ? 1 : 0, BodyTries, *Describe(Me()), *Describe(Foe.Get()));
}

void UMOBContentProbeSubsystem::Finish(bool bOk)
{
	bDone = true;
	LogBodies(TEXT("end"));
	const FString Path = FPaths::ProjectSavedDir() / TEXT("MOB") / FString::Printf(TEXT("cprobe_%s.csv"), *Tag);
	const bool bSaved = FFileHelper::SaveStringToFile(Csv, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_DONE ok=%d saved=%d path=%s frames=%d steps=%d realSec=%.1f"),
		bOk ? 1 : 0, bSaved ? 1 : 0, *Path, Frame, FMath::Max(Step, 0), FPlatformTime::Seconds() - StartRealAt);
	FPlatformMisc::RequestExit(false, TEXT("MOBContentProbe"));
}
