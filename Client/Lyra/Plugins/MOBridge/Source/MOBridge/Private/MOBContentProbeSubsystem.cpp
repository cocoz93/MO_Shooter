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
#include "Components/SphereComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Kismet/KismetSystemLibrary.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "Engine/OverlapResult.h"
#include "UnrealClient.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"

#include "AbilitySystem/Abilities/LyraAbilitySimpleFailureMessage.h"
#include "AbilitySystem/Abilities/LyraGameplayAbility.h"
#include "AbilitySystem/Attributes/LyraHealthSet.h"
#include "AbilitySystem/LyraAbilitySystemComponent.h"
#include "AbilitySystem/Phases/LyraGamePhaseSubsystem.h"
#include "CommonInputModeTypes.h"
#include "Engine/LocalPlayer.h"
#include "Input/CommonUIActionRouterBase.h"
#include "Camera/LyraCameraMode.h"
#include "Character/LyraCharacter.h"
#include "Character/LyraHealthComponent.h"
#include "Equipment/LyraQuickBarComponent.h"
#include "Inventory/LyraInventoryItemDefinition.h"
#include "Inventory/LyraInventoryItemInstance.h"
#include "Inventory/LyraInventoryManagerComponent.h"
#include "GameFramework/GameStateBase.h"
#include "LyraGameplayTags.h"
#include "Messages/LyraVerbMessage.h"
#include "Messages/LyraVerbMessageHelpers.h"
#include "System/LyraGameData.h"
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

	// 무기 판(W-0, weapon-pickup-plan.md) — 받침 자리는 같은 플랜 「받침 자리」 표(dump3.txt, 소수 첫째). Arg = 이 표의 차례
	const FVector WEAPON_PAD_SPOTS[4] = {
		FVector(3377.4, -1830.0, -380.6),	// 0 = B_WeaponSpawner1 소총
		FVector(-3377.4, 1830.0, -380.6),	// 1 = B_WeaponSpawner3 소총
		FVector(-4586.5, 1008.4, -380.4),	// 2 = B_WeaponSpawner7 권총
		FVector(1472.0, -6.7, -1218.1),		// 3 = B_WeaponSpawner5 샷건
	};
	// 탄 줄이기(SetAmmo 의 Arg = 이 표의 차례) — 쏘는 대신 탄 표식을 직접 바꾼다. 받침이 주는 것 · 재장전만 보려고(사격 규칙은 이미 대조 끝)
	struct FAmmoSet { int32 Weapon; int32 Magazine; int32 Spare; };
	const FAmmoSet AMMO_SETS[] = {
		{ 1, 10, 20 },	// 0 = 소총 — 다른 소총 받침에서 예비만 60 까지 차고 탄창 10 은 그대로일 것
		{ 0, 4, 48 },	// 1 = 권총 — 재장전할 거리(탄창 4 · 예비 가득)
		{ 1, 10, 60 },	// 2 = 소총 — 재장전할 거리
		{ 2, 2, 16 },	// 3 = 샷건 — 재장전할 거리
		{ 0, 12, 48 },	// 4 = 권총 가득(대시 판 — 대시 중 사격 칸 앞에 채운다)
	};
	const FStep WEAPONS_SCRIPT[] = {
		{ TEXT("settle"),         1.0f,  EAct::PadsDump,    0.f, false },	// 받침 15개 정보 · 처음 가방(경기 시작 초기화 뒤)
		// 소총 받침에 서 있기 — 집음(새 무기 · 바로 듦) → 꺼짐 → 10초 → 켜짐 → 0.25초 뒤 서 있는 나를 다시 검사(예비 가득 → 실패 · 받침 그대로)
		{ TEXT("rifle_r1"),       14.0f, EAct::WeaponPadOn, 0.f, false },
		{ TEXT("away1"),          1.0f,  EAct::Away,        0.f, false },
		{ TEXT("slot0"),          1.5f,  EAct::SelectSlot,  0.f, false },	// 권총으로 — 꺼내기 몽타주가 끝날 만큼
		{ TEXT("rifle_low"),      0.5f,  EAct::SetAmmo,     0.f, false },
		{ TEXT("rifle_r3"),       2.0f,  EAct::WeaponPadOn, 1.f, false },	// 가진 무기 — 예비만 채움 · 탄창 그대로 · 든 무기(권총) 그대로
		{ TEXT("away2"),          1.0f,  EAct::Away,        0.f, false },
		{ TEXT("pistol_p7"),      2.0f,  EAct::WeaponPadOn, 2.f, false },	// 권총 예비 가득 — 실패 · 받침 그대로
		{ TEXT("away3"),          1.0f,  EAct::Away,        0.f, false },
		{ TEXT("pistol_low"),     0.5f,  EAct::SetAmmo,     1.f, false },
		{ TEXT("reload_press"),   0.1f,  EAct::ReloadPress, 0.f, false },	// 재장전을 켜 두고 —
		{ TEXT("shotgun_s5"),     3.0f,  EAct::WeaponPadOn, 3.f, false },	// — 재장전 중에 새 무기 받침: 넣기만 · 안 듦(IgnorePickUpWhenReload)
		{ TEXT("away4"),          1.0f,  EAct::Away,        0.f, false },
		// 재장전만 — 태그 Event.Movement.Reload 구간 · 탄이 들어온 프레임을 서버 재장전 시간(Shared/Weapon.cpp reloadSeconds)과 견준다
		{ TEXT("pistol_low2"),    0.5f,  EAct::SetAmmo,     1.f, false },
		{ TEXT("reload_pistol"),  3.0f,  EAct::ReloadPress, 0.f, false },
		{ TEXT("slot1"),          1.5f,  EAct::SelectSlot,  1.f, false },
		{ TEXT("rifle_low2"),     0.5f,  EAct::SetAmmo,     2.f, false },
		{ TEXT("reload_rifle"),   3.0f,  EAct::ReloadPress, 0.f, false },
		{ TEXT("slot2"),          1.5f,  EAct::SelectSlot,  2.f, false },
		{ TEXT("shotgun_low"),    0.5f,  EAct::SetAmmo,     3.f, false },
		{ TEXT("reload_shotgun"), 4.0f,  EAct::ReloadPress, 0.f, false },
		// 스스로 죽음 → 부활 뒤 가방(권총 하나일 것). 죽음 ~ 부활 동안에도 칸 시간은 흐른다 — 부활 대기 5초(Shared/MatchRules.h respawnSeconds = GA_AutoRespawn)보다 넉넉히
		{ TEXT("die"),            15.0f, EAct::SelfDestruct, 0.f, false },
		{ TEXT("end"),            1.0f,  EAct::None,        0.f, false },
	};

	// 대시 판(D-0, dash-plan.md) — 연습장 집 둘레에서 고른 자리(PickDashHome, 몸 요 0)에서. 상대는 숨기고 충돌을 꺼 둔다(근접 칸만 다시 켬).
	// 대시 칸은 2초 — DASH_PRESS_AT 에 누르고, 쿨다운 1.5초(GE_HeroDash_Cooldown)가 다음 칸 누름 전에 끝난다. 이동 입력은 누른 뒤 DASH_INPUT_FRAMES 까지 넣고 놓는다(미끄러져 서는 것까지 찍으려고)
	const FStep DASH_SCRIPT[] = {
		{ TEXT("settle"),           1.0f, EAct::Home,           0.f,    false },
		// 선 채 여덟 방향 — Arg = 이동 입력 각(월드 +X 에서 도, +Y = 오른쪽). 대각은 키보드처럼 앞 · 옆 두 입력을 따로 더한다(길이 √2 — 짐작 2 · 3)
		{ TEXT("fwd"),              2.0f, EAct::DashMove,       0.f,    false },
		{ TEXT("back"),             2.0f, EAct::DashMove,       180.f,  false },
		{ TEXT("left"),             2.0f, EAct::DashMove,       -90.f,  false },
		{ TEXT("right"),            2.0f, EAct::DashMove,       90.f,   false },
		{ TEXT("fwd_left"),         2.0f, EAct::DashMove,       -45.f,  false },
		{ TEXT("fwd_right"),        2.0f, EAct::DashMove,       45.f,   false },
		{ TEXT("back_left"),        2.0f, EAct::DashMove,       -135.f, false },
		{ TEXT("back_right"),       2.0f, EAct::DashMove,       135.f,  false },
		// 입력 없이 — Arg = 몸 요(짐작 1)
		{ TEXT("still_yaw0"),       2.0f, EAct::DashNoInput,    0.f,    false },
		{ TEXT("still_yaw90"),      2.0f, EAct::DashNoInput,    90.f,   false },
		// 위 · 아래 보고 앞 — Arg = 피치(+ = 위). 앞 대시 방향 = 카메라 앞 × (1, 1, 1.2). 3.5초 = 내려앉기까지(dash1 위 45 는 832cm 떠 2초 칸 끝에 아직 공중이었다)
		{ TEXT("pitch_up20"),       3.5f, EAct::DashPitch,      20.f,   false },
		{ TEXT("pitch_up45"),       3.5f, EAct::DashPitch,      45.f,   false },
		{ TEXT("pitch_down20"),     3.5f, EAct::DashPitch,      -20.f,  false },
		// 점프 꼭대기 — 점프는 능력 입력(InputTag.Jump, GA_Hero_Jump)으로 누르고, 떨어지기 시작한 프레임에 대시
		{ TEXT("jump_apex"),        2.5f, EAct::DashJumpApex,   0.f,    false },
		// 앉은 채 — 대시가 일으켜 세우나. 안 섰으면 stand 칸이 세운다
		{ TEXT("crouch"),           0.8f, EAct::CrouchToggle,   0.f,    false },
		{ TEXT("crouch_fwd"),       2.0f, EAct::DashMove,       0.f,    false },
		{ TEXT("stand"),            0.8f, EAct::StandUp,        0.f,    false },
		// 매 프레임 누름 — 다시 켜지는 간격(짐작 4). 대시가 끝날 때마다 집으로 되돌린다(길이 막히지 않게)
		{ TEXT("spam"),             5.0f, EAct::DashSpam,       0.f,    false },
		{ TEXT("spam_cool"),        1.6f, EAct::Home,           0.f,    false },
		// 대시 중 누름 — Arg = DASH_ACTIONS 차례. 대시가 켜진 프레임 + DASH_ACTION_AFTER 에 누르고, 탭은 다음 프레임에 · 쥠은 칸 끝에 뗀다
		{ TEXT("ammo_full1"),       0.2f, EAct::SetAmmo,        4.f,    false },
		{ TEXT("during_fire_tap"),  2.0f, EAct::DashThen,       0.f,    false },
		{ TEXT("ammo_full2"),       0.2f, EAct::SetAmmo,        4.f,    false },
		{ TEXT("during_fire_hold"), 2.0f, EAct::DashThen,       1.f,    false },
		{ TEXT("ammo_low1"),        0.2f, EAct::SetAmmo,        1.f,    false },
		{ TEXT("during_reload"),    2.0f, EAct::DashThen,       2.f,    false },
		{ TEXT("during_melee"),     2.0f, EAct::DashThen,       3.f,    false },
		{ TEXT("during_jump_tap"),  2.0f, EAct::DashThen,       4.f,    false },
		{ TEXT("during_jump_hold"), 2.0f, EAct::DashThen,       5.f,    false },
		{ TEXT("during_ads_hold"),  2.0f, EAct::DashThen,       6.f,    false },
		{ TEXT("ads_settle"),       0.6f, EAct::Home,           0.f,    false },
		// 쥔 채 대시 — 칸 첫 프레임부터 쥐고(Arg = DASH_ACTIONS 차례) DASH_PRESS_AT 에 대시, 칸 끝에 뗀다(켜는 순간 끊나 · 끝난 뒤 다시 켜지나 — 플랜 「막기 · 끊기」 줄)
		{ TEXT("ads_then_dash"),    2.0f, EAct::HoldThenDash,   6.f,    false },
		{ TEXT("ads_settle2"),      0.6f, EAct::Home,           0.f,    false },
		{ TEXT("ammo_full4"),       0.2f, EAct::SetAmmo,        4.f,    false },
		{ TEXT("fire_then_dash"),   2.0f, EAct::HoldThenDash,   1.f,    false },
		// 재장전 중 대시 — 칸 첫 프레임에 재장전, DASH_PRESS_AT 에 대시(재장전이 끊기나 · 탄이 들어오나)
		{ TEXT("ammo_low2"),        0.2f, EAct::SetAmmo,        1.f,    false },
		{ TEXT("reload_dash"),      2.0f, EAct::ReloadDash,     0.f,    false },
		{ TEXT("ammo_full3"),       0.2f, EAct::SetAmmo,        4.f,    false },
		// 근접 맞은 직후 대시(밀기와 대시 힘 겹침) — 상대를 앞 Arg cm 에 세우고(맞는 앞 범위 150~520, gym1) MELEE_PRESS_AT 에 근접.
		// 근접을 누른 프레임부터 오른쪽(+Y) 입력(밀기는 앞 X, 대시는 옆 Y — 두 힘을 축으로 가른다), 근접이 켜진 프레임에 대시를 누른다(켜지는 건 다음 프레임)
		{ TEXT("foe_show"),         0.3f, EAct::FoeShow,        0.f,    false },
		{ TEXT("foe_setup"),        0.5f, EAct::FoeSetup,       0.f,    false },
		{ TEXT("melee_dash"),       2.0f, EAct::MeleeDash,      300.f,  false },
		{ TEXT("foe_away"),         1.0f, EAct::FoeAway,        0.f,    false },
		// 벽 앞 — 집에서 가장 가까운 벽(ClearAround) 쪽으로, 캡슐 앞 Arg cm 에 벽을 두고 그쪽으로 대시
		{ TEXT("wall"),             2.0f, EAct::DashWall,       150.f,  false },
		// 같은 프레임 앉기 + 대시
		{ TEXT("crouch_same"),      2.0f, EAct::DashCrouchSame, 0.f,    false },
		{ TEXT("stand2"),           0.8f, EAct::StandUp,        0.f,    false },
		// 누르는 프레임에 입력을 바꿈(D-2) — Arg = 바꾼 입력 각. 켜질 때의 마지막 이동 입력이 앞 프레임 것(앞)이면 앞 몽타주 · 카메라 앞, 이번 것이면 그 각의 몽타주.
		// 앞 칸들의 프레임 번호를 dash5 와 같게 두려고 끝 칸(end) 바로 앞에 붙였다
		{ TEXT("turn_left"),        2.0f, EAct::DashTurn,       -90.f,  false },
		{ TEXT("turn_back"),        2.0f, EAct::DashTurn,       180.f,  false },
		{ TEXT("end"),              0.5f, EAct::Home,           0.f,    false },
	};
	constexpr float DASH_PRESS_AT = 0.3f;		// 칸 시작에서 대시 누르기까지 — 걸음이 붙을 만큼(제안값, 근접 누름 MELEE_PRESS_AT 과 같음)
	constexpr int32 DASH_INPUT_FRAMES = 33;		// 누른 뒤 이동 입력을 넣는 프레임 수 — 능력 켜짐 0.55초(AbilityDuration) × 60(원본 기준. 실측 켜짐은 31프레임이라 넉넉히 덮는다)
	constexpr int32 DASH_ACTION_AFTER = 5;		// 대시 중 누름 — 켜진 뒤 몇 프레임에(힘 0.25초 안)
	constexpr double DASH_CLEAR_TRACE = 3000.0;	// ClearAround 쓸기 길이
	// 대시 판 집 고르기 — 여덟 방향이 이만큼 비어야 한다. dash1 에서 누른 자리부터 칸 끝까지 간 거리 최대 939cm(위 20 앞) + 누르기 전 걸음 약 60cm 위에 얹은 값
	constexpr double DASH_CLEAR_NEED = 1100.0;
	constexpr double DASH_WALL_NORMAL_Z = 0.1;	// 벽 칸이 벽으로 보는 면 — 법선 Z 절댓값이 이 아래(거의 서 있는 면)
	constexpr double DASH_CLEAR_LIFT = 5.0;		// 쓸 때 캡슐을 띄우는 높이 — 바닥은 안 긁고 낮은 경사 · 턱은 걸리게(dash1 은 걸음 턱 높이 MaxStepHeight 만큼 띄워 뒤 오른쪽 경사를 못 봤다)
	constexpr double DASH_HOME_RING = 300.0;	// 후보 자리 — 연습장 집에서 이 간격의 고리 넷 × 여덟 방향
	constexpr int32 DASH_HOME_RINGS = 4;
	// 대시 중 누름 — 입력 표식 · 그 능력 클래스 이름 조각 · 쥠 여부. 재장전은 「GA_Weapon_Reload_」(권총 · 소총 · 샷건 재장전만 — 「Reload」만 보면
	// GA_Weapon_AutoReload 도 걸린다, 이름 목록 MOBWeaponDumpSubsystem.cpp ASSETS)
	struct FDashAction { const TCHAR* Name; const TCHAR* InputTag; const TCHAR* Ability; bool bHold; };
	const FDashAction DASH_ACTIONS[] = {
		{ TEXT("fire_tap"),  TEXT("InputTag.Weapon.Fire"),   TEXT("GA_Weapon_Fire"), false },
		{ TEXT("fire_hold"), TEXT("InputTag.Weapon.Fire"),   TEXT("GA_Weapon_Fire"), true  },
		{ TEXT("reload"),    TEXT("InputTag.Weapon.Reload"), TEXT("GA_Weapon_Reload_"), false },
		{ TEXT("melee"),     TEXT("InputTag.Ability.Melee"), TEXT("GA_Melee"),       false },
		{ TEXT("jump_tap"),  TEXT("InputTag.Jump"),          TEXT("GA_Hero_Jump"),   false },
		{ TEXT("jump_hold"), TEXT("InputTag.Jump"),          TEXT("GA_Hero_Jump"),   true  },
		{ TEXT("ads_hold"),  TEXT("InputTag.Weapon.ADS"),    TEXT("GA_ADS"),         true  },
	};
	const FDashAction& DashActionAt(float Arg) { return DASH_ACTIONS[FMath::Clamp((int32)Arg, 0, (int32)UE_ARRAY_COUNT(DASH_ACTIONS) - 1)]; }
	// 칸에 대시 중 누름 능력(DASH_ACTIONS[Arg])이 있나 — 대시 중 · 쥔 채 · L-1-c 같은 프레임 둘
	bool HasDashAction(EAct A) { return A == EAct::DashThen || A == EAct::HoldThenDash || A == EAct::ActDashSame || A == EAct::DashActSame; }
	// L-1-c 근접 칸 상대 거리 — 돌진 없이 맞는 거리(e1g3 melee_150 hit=1 · 피해 100 · lunge2d 0)
	constexpr double L1C_MELEE_FOE = 150.0;
	// 키보드 같은 이동 입력 — 각을 축마다 반올림(45도 = (1, 1), 길이 √2. Lyra 영웅 입력은 앞 · 옆을 따로 더한다)
	FVector KeyInput(float AngleDeg)
	{
		const double A = FMath::DegreesToRadians((double)AngleDeg);
		return FVector(FMath::RoundToDouble(FMath::Cos(A)), FMath::RoundToDouble(FMath::Sin(A)), 0.0);
	}

	// 수류탄 판(G-0, grenade-plan.md) — 대시 판과 같은 집(PickDashHome, 여덟 방향 1100cm 빔). 상대는 숨겨 두고(사람 칸만 다시 켬) 던짐 칸마다 나 · 상대 체력을 1000 으로 둔다(피해 값만 본다).
	// 던짐 칸은 5.1초 — GREN_PRESS_AT 에 Q 를 한 프레임 누른다. 쿨다운 5초(GE_Grenade_Cooldown) = 300프레임이 다음 칸 누름(306프레임 뒤) 전에 끝나고, 타이머 2초도 칸 안에 든다
	// SelfNear(G-0-b2) = 피해 칸과 같은 세움을 상대 대신 나에게(자기 피해 · 자기 처치)
	// Fixed(L-1-a) = 정한 자리(GREN_FIXED[Arg])에 서서 정한 점을 겨누거나 정한 요 · 피치로 던진다 — 폭발 칸이 아니라 맨 뒤에 붙인다(앞 값 그대로, GPLACE place= 칸)
	// Placed(L-1-a) = 집에서 수평으로 던지고(Open 피치 0 과 같음) 생긴 그 프레임(내 틱 — 나이 0 줄을 적은 뒤)에 GREN_PLACED[Arg] 자리 · 속도로 옮긴다(GDROP 줄, 회전은 그대로)
	// WallAt(L-1-b) = 피해 칸 — 던진 수류탄을 생긴 프레임에 GREN_PLACED[GREN_WALLAT[Arg].Drop] 으로 옮겨 떨구고, 세울 나이(GREN_BLAST_AT)에 GREN_WALLAT[Arg] 자리로 옮겨 상대를 그 표의 자리(바닥은 수류탄 높이 둘레에서 찾음)에 세운다(GWALLSET 줄)
	enum class EGrenPlace : uint8 { Open, WallCam, WallGap, WallBack, FoeAt, Slope, Corner, BlastHit, BlastCap, BlastCenter, BlastCrouch, WallLedge, WallRoof, SelfNear, Fixed, Placed, WallAt };
	bool IsBlastPlace(EGrenPlace P) { return (P >= EGrenPlace::BlastHit && P <= EGrenPlace::SelfNear) || P == EGrenPlace::WallAt; }
	// 뒤 셋(G-0-b2) — 칸 시작 체력(0 = 1000) · 수류탄이 생기고 이 프레임 뒤 스스로 죽음(−1 = 안 죽음). 최대 체력은 늘 1000
	// 마지막(G-0-b3) — 수류탄이 생기고 이 프레임 뒤 상대가 나를 죽임(−1 = 안 함)
	// G-0-c 섞어 누름(묶음 13~16) — Spam = 칸 안 GREN_PRESS_AT ~ GREN_SPAM_UNTIL 동안 매 프레임 Q(떼지 않음) · QFire / FireQ = 한 프레임에 Q 와 사격 탭(앞이 먼저 누름) ·
	// ReloadQ · DashQ · MeleeQ = 그 능력을 누르고, 켜진 뒤 GrenComboSpec 의 QAfter 프레임에 Q
	// L-1-c(미결 5 · 22) — …Same = 한 프레임 안에 두 누름(이름 차례 = 누르는 차례, 켜진 뒤 기다림 없음)
	enum class EGrenCombo : uint8 { None, Spam, QFire, FireQ, ReloadQ, DashQ, MeleeQ, MeleeQSame, QMeleeSame, ReloadQSame, QReloadSame };
	// G-0-c2(묶음 17 · 짐작 7) — Drop = 첫 수류탄이 멈추면 둘째를 던져 첫 수류탄 바로 위로 옮기고 아래로 · Walk = 멈춘 수류탄 뒤로 나를 옮겨 그 위로 걷기 ·
	// WalkBare = 수류탄 없이 Walk 와 같은 자리 · 같은 걷기(대조군 — 같은 인덱스 내 자리 차가 수류탄 탓)
	enum class EGrenTouch : uint8 { None, Drop, Walk, WalkBare };
	struct FGrenShot { const TCHAR* Name; EGrenPlace Place; float Pitch; float Arg; bool bRun; bool bJump; float FoeHp = 0.f; float MyHp = 0.f; int32 DieAfter = -1; int32 FoeKillAfter = -1;
		EGrenCombo Combo = EGrenCombo::None; EGrenTouch Touch = EGrenTouch::None; };
	const FGrenShot GREN_SHOTS[] = {
		// 묶음 1 · 2 — 방향 · 피치(집, 몸 요 0). 하늘 둘(선이 안 맞는 갈래 — 짐작 1) · 수평 · 먼 바닥 둘 · 발밑(카메라가 위로 올라가 선 시작점도 바닥 위 — g0a4 카메라 z 542 · 시작 z 약 152 · 맞은 거리 118)
		{ TEXT("sky_p45"),      EGrenPlace::Open,     45.f,  0.f,    false, false },	// 0
		{ TEXT("sky_p60"),      EGrenPlace::Open,     60.f,  0.f,    false, false },	// 1
		{ TEXT("p0"),           EGrenPlace::Open,     0.f,   0.f,    false, false },	// 2
		{ TEXT("floor_m20"),    EGrenPlace::Open,     -20.f, 0.f,    false, false },	// 3
		{ TEXT("floor_m35"),    EGrenPlace::Open,     -35.f, 0.f,    false, false },	// 4 바닥 → 멈춤(묶음 3)
		{ TEXT("feet_m60"),     EGrenPlace::Open,     -60.f, 0.f,    false, false },	// 5
		// 달리며 · 점프 중 · 앉은 채(앉기는 각본 칸이 켠다)
		{ TEXT("run_p0"),       EGrenPlace::Open,     0.f,   0.f,    true,  false },	// 6
		{ TEXT("jump_p0"),      EGrenPlace::Open,     0.f,   0.f,    false, true  },	// 7
		{ TEXT("crouch_p0"),    EGrenPlace::Open,     0.f,   0.f,    false, false },	// 8
		// 가까운 벽 — Arg = 카메라에서 벽까지(시선 방향). 350 = 선 시작점(450)보다 안(처음 300 은 캡슐이 벽에 닿아도 321 까지라 못 맞췄다 — g0a1) · 480 · 510 = 맞은 거리 30 · 60(「< 45」 경계 앞뒤). 1000 = 벽 튕김(묶음 3)
		{ TEXT("wall_cam350"),  EGrenPlace::WallCam,  0.f,   350.f,  false, false },	// 9
		{ TEXT("wall_cam480"),  EGrenPlace::WallCam,  0.f,   480.f,  false, false },	// 10
		{ TEXT("wall_cam510"),  EGrenPlace::WallCam,  0.f,   510.f,  false, false },	// 11
		{ TEXT("wall_cam1000"), EGrenPlace::WallCam,  0.f,   1000.f, false, false },	// 12
		// 던진 사람 바로 앞 벽(캡슐 겉면 2cm — 0.16초 안에 벽, 되돌아오면 내 몸 · 묶음 4 · 6 자기) · 등이 벽(카메라가 벽에 밀림 — 묶음 18)
		{ TEXT("wall_front"),   EGrenPlace::WallGap,  0.f,   2.f,    false, false },	// 13
		{ TEXT("wall_back"),    EGrenPlace::WallBack, 0.f,   2.f,    false, false },	// 14
		// 비스듬한 면(미끄러짐) · 구석(두 벽) — FindSurface 가 찾은 점을 겨눈다(구석은 그 점을 대각으로 보는 자리로 옮겨서 · 묶음 3)
		{ TEXT("slope"),        EGrenPlace::Slope,    0.f,   0.f,    false, false },	// 15
		{ TEXT("corner"),       EGrenPlace::Corner,   0.f,   0.f,    false, false },	// 16
		// 사람 — Arg = 앞 거리. 800 은 몸 가운데를 겨눔(묶음 1 사람 몸 · 6 적이면 바로 터짐 / 같은 팀이면 튕김), 72 는 캡슐이 2cm 떨어진 바로 앞 적(던진 사람 캡슐 안에서 터짐)
		{ TEXT("foe_body"),     EGrenPlace::FoeAt,    0.f,   800.f,  false, false },	// 17
		{ TEXT("foe_close"),    EGrenPlace::FoeAt,    0.f,   72.f,   false, false },	// 18
		{ TEXT("foe_same"),     EGrenPlace::FoeAt,    0.f,   800.f,  false, false },	// 19
		// 피해 칸(G-0-b · 묶음 7 · 8 · 9 · 10) — 집에서 피치 −20 으로 던져(멈춤 약 46프레임 뒤 · g0a4) 터지기 전(GREN_BLAST_AT)에 상대를 수류탄 둘레에 세운다. 상대는 집 반대(+X)쪽.
		// BlastHit = 폭발 선(수류탄 → 상대 몸 가운데)이 상대 캡슐에 맞는 거리가 Arg · BlastCap = 캡슐 겉면까지 가장 짧은 거리가 Arg(겹침 450 경계 — 짐작 6) ·
		// BlastCenter · BlastCrouch = 몸 가운데까지 수평 Arg(서서 · 앉아 · 같은 팀) · WallLedge · WallRoof = 벽 뒤(짐작 2 — Arg = 벽 면에서 상대 몸 가운데까지 수평)
		{ TEXT("blast_hit45"),      EGrenPlace::BlastHit,    -20.f, 45.f,  false, false },	// 20
		{ TEXT("blast_hit123"),     EGrenPlace::BlastHit,    -20.f, 123.f, false, false },	// 21
		{ TEXT("blast_hit225"),     EGrenPlace::BlastHit,    -20.f, 225.f, false, false },	// 22
		{ TEXT("blast_hit449"),     EGrenPlace::BlastHit,    -20.f, 449.f, false, false },	// 23
		{ TEXT("blast_hit451"),     EGrenPlace::BlastHit,    -20.f, 451.f, false, false },	// 24
		{ TEXT("blast_cap449"),     EGrenPlace::BlastCap,    -20.f, 449.f, false, false },	// 25
		{ TEXT("blast_cap451"),     EGrenPlace::BlastCap,    -20.f, 451.f, false, false },	// 26
		{ TEXT("blast_wall_ledge"), EGrenPlace::WallLedge,   -20.f, 150.f, false, false },	// 27
		{ TEXT("blast_wall_roof"),  EGrenPlace::WallRoof,    -20.f, 100.f, false, false },	// 28
		{ TEXT("blast_stand200"),   EGrenPlace::BlastCenter, -20.f, 200.f, false, false },	// 29
		{ TEXT("blast_crouch200"),  EGrenPlace::BlastCrouch, -20.f, 200.f, false, false },	// 30
		{ TEXT("blast_same200"),    EGrenPlace::BlastCenter, -20.f, 200.f, false, false },	// 31 같은 팀(앞 칸 FoeSameTeam 그대로)
		// G-0-b2 죽음 · 부활 길(묶음 10 무적 · 11 · 12 · 19) — 상대 처치(체력 50 ← 수평 200 피해 93.114, g0b · 처치 소식 · 처치 아이콘 사진) ·
		// 갓 부활한 상대 몸 겨눔(직접 맞음 — 상대가 다시 생기고 50프레임 뒤 터짐, 부활 무적 166프레임 안 · g0life1 실측) ·
		// 던진 뒤 죽음(생기고 DieAfter 프레임 뒤 스스로 죽음 — 신관으로 터지는 것 / 날아가다 상대에 직접 맞는 것, 짐작 3) · 자기 피해(그 뒤 상대가 나를 죽임 — 도움) · 자기 처치
		{ TEXT("g_kill_foe"),       EGrenPlace::BlastCenter, -20.f, 200.f, false, false, 50.f, 0.f,  -1 },	// 32
		{ TEXT("g_immune"),         EGrenPlace::FoeAt,       0.f,   800.f, false, false, 0.f,  0.f,  -1 },	// 33
		{ TEXT("g_die_after"),      EGrenPlace::BlastCenter, -20.f, 200.f, false, false, 0.f,  0.f,  5 },		// 34
		{ TEXT("g_die_direct"),     EGrenPlace::FoeAt,       0.f,   800.f, false, false, 0.f,  0.f,  2 },		// 35
		{ TEXT("g_self_hurt"),      EGrenPlace::SelfNear,    -20.f, 200.f, false, false, 0.f,  0.f,  -1 },	// 36
		{ TEXT("g_self_kill"),      EGrenPlace::SelfNear,    -20.f, 200.f, false, false, 0.f,  50.f, -1 },	// 37
		// G-0-b3 경기 끝(묶음 20) — 목표 점수를 낮춘 뒤(LowerTarget) g_kill_foe 와 같은 세움(수평 200 · 체력 50 인 상대)에 던지고, 수류탄이 생기고 40프레임 뒤 상대가 나를 죽여
		// 상대 팀이 이긴다 → 수류탄은 끝 단계에서 터진다(40 = 상대를 세우는 96 · 신관 120 보다 앞이라 끝 단계가 된 뒤 세우고 터진다. 진행 중이면 g_kill_foe 처럼 상대가 죽는다) ·
		// 끝 단계에서 내가 다시 생긴 뒤 상대 몸 겨눔(g_immune 과 같은 직접 맞음 — 던져지나 · 피해)
		{ TEXT("g_end_inflight"),   EGrenPlace::BlastCenter, -20.f, 200.f, false, false, 50.f, 0.f,  -1, 40 },	// 38
		{ TEXT("g_end_throw"),      EGrenPlace::FoeAt,       0.f,   800.f, false, false, 0.f,  0.f,  -1, -1 },	// 39
		// G-0-c 섞어 누름(묶음 13~16) — 모두 집에서 피치 0(p0 와 같은 세움 — 멀리 떨어져 자기 피해 없음, g0a4 p0 myDmg 0)
		{ TEXT("c_spam"),           EGrenPlace::Open,        0.f,   0.f,   false, false, 0.f,  0.f,  -1, -1, EGrenCombo::Spam },		// 40
		{ TEXT("c_q_fire"),         EGrenPlace::Open,        0.f,   0.f,   false, false, 0.f,  0.f,  -1, -1, EGrenCombo::QFire },	// 41
		{ TEXT("c_fire_q"),         EGrenPlace::Open,        0.f,   0.f,   false, false, 0.f,  0.f,  -1, -1, EGrenCombo::FireQ },	// 42
		{ TEXT("c_reload_q"),       EGrenPlace::Open,        0.f,   0.f,   false, false, 0.f,  0.f,  -1, -1, EGrenCombo::ReloadQ },	// 43
		{ TEXT("c_dash_q"),         EGrenPlace::Open,        0.f,   0.f,   false, false, 0.f,  0.f,  -1, -1, EGrenCombo::DashQ },	// 44
		{ TEXT("c_melee_q"),        EGrenPlace::Open,        0.f,   0.f,   false, false, 0.f,  0.f,  -1, -1, EGrenCombo::MeleeQ },	// 45
		// G-0-c2(묶음 17) — 피치 −20(floor_m20 과 같은 세움 — 생기고 46프레임에 집 앞 약 9m 바닥에 멈춘다, g0c2 · 터짐 120 까지 74프레임)
		{ TEXT("c2_drop"),          EGrenPlace::Open,        -20.f, 0.f,   false, false, 0.f,  0.f,  -1, -1, EGrenCombo::None, EGrenTouch::Drop },		// 46
		{ TEXT("c2_walk"),          EGrenPlace::Open,        -20.f, 0.f,   false, false, 0.f,  0.f,  -1, -1, EGrenCombo::None, EGrenTouch::Walk },		// 47
		{ TEXT("c2_walk_bare"),     EGrenPlace::Open,        -20.f, 0.f,   false, false, 0.f,  0.f,  -1, -1, EGrenCombo::None, EGrenTouch::WalkBare },	// 48
		// L-1-a(lyra-match-plan, 미결 12 · 19 · 20) — 정한 자리 칸(Arg = GREN_FIXED 차례). G-0 각본엔 안 들고 GRENADE_L1A_SCRIPT 만 쓴다
		{ TEXT("a_corner_z110"),    EGrenPlace::Fixed,       0.f,   0.f,   false, false },	// 49
		{ TEXT("a_corner_z200"),    EGrenPlace::Fixed,       0.f,   1.f,   false, false },	// 50
		{ TEXT("a_corner_z60"),     EGrenPlace::Fixed,       0.f,   2.f,   false, false },	// 51
		{ TEXT("a_corner_xside"),   EGrenPlace::Fixed,       0.f,   3.f,   false, false },	// 52
		{ TEXT("a_corner_yside"),   EGrenPlace::Fixed,       0.f,   4.f,   false, false },	// 53
		{ TEXT("a_graze_x960"),     EGrenPlace::Fixed,       0.f,   5.f,   false, false },	// 54
		{ TEXT("a_graze_x940"),     EGrenPlace::Fixed,       0.f,   6.f,   false, false },	// 55
		{ TEXT("a_graze_x920"),     EGrenPlace::Fixed,       0.f,   7.f,   false, false },	// 56
		{ TEXT("a_graze_x900"),     EGrenPlace::Fixed,       0.f,   8.f,   false, false },	// 57
		{ TEXT("a_down_m60"),       EGrenPlace::Fixed,       0.f,   9.f,   false, false },	// 58
		{ TEXT("a_down_m45"),       EGrenPlace::Fixed,       0.f,   10.f,  false, false },	// 59
		{ TEXT("a_down_m75"),       EGrenPlace::Fixed,       0.f,   11.f,  false, false },	// 60
		{ TEXT("a_down_roof_m60"),  EGrenPlace::Fixed,       0.f,   12.f,  false, false },	// 61
		{ TEXT("a_step_z255"),      EGrenPlace::Fixed,       0.f,   13.f,  false, false },	// 62
		{ TEXT("a_step_z300"),      EGrenPlace::Fixed,       0.f,   14.f,  false, false },	// 63
		{ TEXT("a_step_ledge"),     EGrenPlace::Fixed,       0.f,   15.f,  false, false },	// 64
		// L-1-a 옮겨 놓기(Arg = GREN_PLACED 차례) — 던짐만으로는 못 맞추는 갈래(미끄러짐 · 여러 번 맞음 · 구석 · 겹침 풀기 둘)
		{ TEXT("p_floor_slide"),    EGrenPlace::Placed,      0.f,   0.f,   false, false },	// 65
		{ TEXT("p_floor_multi"),    EGrenPlace::Placed,      0.f,   1.f,   false, false },	// 66
		{ TEXT("p_floorwall_c1"),   EGrenPlace::Placed,      0.f,   2.f,   false, false },	// 67
		{ TEXT("p_floorwall_c2"),   EGrenPlace::Placed,      0.f,   3.f,   false, false },	// 68
		{ TEXT("p_wall_slide_g05"), EGrenPlace::Placed,      0.f,   4.f,   false, false },	// 69
		{ TEXT("p_wall_slide_g1"),  EGrenPlace::Placed,      0.f,   5.f,   false, false },	// 70
		{ TEXT("p_crease_move1"),   EGrenPlace::Placed,      0.f,   6.f,   false, false },	// 71
		{ TEXT("p_crease_move2"),   EGrenPlace::Placed,      0.f,   7.f,   false, false },	// 72
		{ TEXT("p_crease_comb1"),   EGrenPlace::Placed,      0.f,   8.f,   false, false },	// 73
		{ TEXT("p_crease_comb2"),   EGrenPlace::Placed,      0.f,   9.f,   false, false },	// 74
		// L-1-b(lyra-match-plan, 미결 17 · 맵 L_Expanse) — 이동은 지나고 수류탄은 막는 창틀 띠(GDIFF grenOnly)에 옮겨 놓기 셋(GREN_PLACED 10~12) · 무기 광선은 지나고 폭발 선은 막는 문틀(blastOnly) 뒤 · 같은 거리 대조(GREN_WALLAT 0 · 1,
		// 폭발 칸 피치 −60 은 던짐만 일으키고, 생긴 프레임에 GREN_PLACED 13 으로 옮겨 떨군다 — 나이 57 에 멈춤, 세움 GREN_BLAST_AT 96 앞). GRENADE_L1B_SCRIPT 만 쓴다
		{ TEXT("b_gonly650"),       EGrenPlace::Placed,      0.f,   10.f,  false, false },	// 75
		{ TEXT("b_gonly650_slant"), EGrenPlace::Placed,      0.f,   11.f,  false, false },	// 76
		{ TEXT("b_gonly647"),       EGrenPlace::Placed,      0.f,   12.f,  false, false },	// 77
		{ TEXT("b_blast623"),       EGrenPlace::WallAt,      -60.f, 0.f,   false, false },	// 78
		{ TEXT("b_blast_open"),     EGrenPlace::WallAt,      -60.f, 1.f,   false, false },	// 79
		// L-1-c(미결 5 · 22) — 같은 프레임 두 누름, 이름 차례 = 누르는 차례. GRENADE_L1C_SCRIPT 만 쓴다. 근접 둘은 상대를 앞 L1C_MELEE_FOE 에 세우고(FoeAt — 200 안이라 겨누지 않고 피치만)
		// 피치 45 로 상대 위로 던진다(근접 피해만 보려고). 재장전 둘은 상대를 숨긴 채 집에서 피치 0(p0 · c_q_fire 와 같은 세움)
		{ TEXT("s_melee_q"),        EGrenPlace::FoeAt,       45.f,  150.f, false, false, 0.f,  0.f,  -1, -1, EGrenCombo::MeleeQSame },	// 80
		{ TEXT("s_q_melee"),        EGrenPlace::FoeAt,       45.f,  150.f, false, false, 0.f,  0.f,  -1, -1, EGrenCombo::QMeleeSame },	// 81
		{ TEXT("s_reload_q"),       EGrenPlace::Open,        0.f,   0.f,   false, false, 0.f,  0.f,  -1, -1, EGrenCombo::ReloadQSame },	// 82
		{ TEXT("s_q_reload"),       EGrenPlace::Open,        0.f,   0.f,   false, false, 0.f,  0.f,  -1, -1, EGrenCombo::QReloadSame },	// 83
	};
	// L-1-a 옮겨 놓을 자리 · 속도 — 값은 Shared 탄체 걸음으로 골랐다(리포 밖 C:\tmp\mob\lm\l1a\search — 집 수평 던짐 회전 1.1957 · 1.5889 · 0 의 캡슐로
	// 면까지 틈을 재고, 소수 여섯째 자리로 자른 값 그대로 걸어 그 갈래가 나고 이웃 값에서도 나는 자리). 정답지는 이 값의 Lyra 기록이다.
	// 이 각본의 실제 던짐 회전은 1.1978 · 1.5975(l1a1 GSPAWN) — 고를 때 쓴 회전과 0.01도 안쪽으로 달라 틈이 조금 다르다. 대조는 기록 회전으로 한다(벽 4 · 5 는 이 기록 회전으로 다시 골랐다)
	//   바닥 = 물체 0 윗면 z 50(열린 바닥 −800, −1300) · 벽 밑동 = 물체 10 북쪽 면 y −2000 · 벽 = 지붕(물체 6) 서쪽 면 x 1000 · 구석 = 그 면과 둔덕(물체 8) 북쪽 면 y −1000
	//   0 · 1 바닥 틈 0.01 미끄러짐(+x 2400 · +x 1500 아래 15 → 여러 번 맞음) / 2 · 3 바닥 틈 0.0005 · 벽 밑동까지 0.2 · 1.0 에서 −y 1500(구석)
	//   4 · 5 벽 틈 0.5 · 1 에서 −y 2000 · 면 쪽 10 · 12(벽 미끄러짐 — 처음 값 면 쪽 1(틈 0.5) · 5(틈 2)는 얕은 각이 벽 자리 6e-6cm 차를
	//   3,000 배로 키워 대조 기준을 넘었다. 이 값은 벽을 ±2e-5 옮겨도 궤적 차 0.00002) / 6~9 구석 앞 공중에서 아래로 500 — 0.16초 뒤 회전이 속도를 따라 눕으며 두 면에 겹침(풀기+원래 옮김 · 두 법선 더함)
	struct FGrenPlaced { double X, Y, Z, VX, VY, VZ; };
	const FGrenPlaced GREN_PLACED[] = {
		{ -800.0,     -1300.0,     52.010653, 2400.0, 0.0,     0.0 },
		{ -800.0,     -1300.0,     52.010653, 1500.0, 0.0,     -15.0 },
		{ 250.0,      -1994.793635, 52.001153, 0.0,   -1500.0, 0.0 },
		{ 250.0,      -1993.993635, 52.001153, 0.0,   -1500.0, 0.0 },
		{ 994.562575, -200.0,      250.0,     10.0,   -2000.0, 0.0 },
		{ 994.062575, -200.0,      250.0,     12.0,   -2000.0, 0.0 },
		{ 994.562576, -996.993635, 150.0,     0.0,    0.0,     -500.0 },
		{ 994.562576, -998.993635, 150.0,     0.0,    0.0,     -500.0 },
		{ 994.562576, -994.493635, 150.0,     0.0,    0.0,     -500.0 },
		{ 997.062576, -994.493635, 150.0,     0.0,    0.0,     -500.0 },
		// L-1-b(L_Expanse) — 창틀 650 · 647 의 벽 앞 1cm 띠(y −1674 면, x −315~−314 · 313~314, z −846~−147 — 뒤 벽 621 · 629 면 y −1675 보다 1cm 앞, 깃발 WbGB = 이동만 지나감)를 −y 로 맞힌다.
		// 값은 Shared 탄체 걸음으로 골랐다(리포 밖 C:\tmp\mob\lm\l1b\search) — 나이 15 에 띠 가운데(x −314.5 · 313.5)에 법선 +y 로 맞음. 0.16초 뒤라 회전이 속도를 따라
		// 캡슐 축이 수평으로 안 기울어 던짐 회전과 상관없다. 비스듬(−x 400)은 시작 x 를 ±0.6 옮기면 띠 끝(법선 x ±0.02)에 맞아 그 가운데로 골랐다
		{ -314.5,     -1420.0,     -480.0,    0.0,    -1000.0, 0.0 },
		{ -214.9,     -1420.0,     -480.0,    -400.0, -1000.0, 0.0 },
		{ 313.5,      -1420.0,     -480.0,    0.0,    -1000.0, 0.0 },
		// 13 = L-1-b 폭발 칸(WallAt) 떨굼 — 집에서 던지면 카메라 선이 앞 118cm 바닥을 맞혀 피치 −38 · 요 23.7 로 나가 비탈 BlockingVolume 에 튀고 나이 96 까지 안 멈췄다(l1b1 첫 판 GAIM · GHIT).
		// 그래서 생긴 프레임에 집 옆 빈 바닥 위 141cm 로 옮겨 그냥 떨군다 — Shared 걸음(던짐 회전 −38.11 · 23.69)으로 나이 32 · 51 · 57 바닥 783, 57 에 멈춤. x · y · z ±1 도 같은 나이
		{ 800.0,      -1500.0,     -700.0,    0.0,    0.0,     0.0 },
	};
	const FGrenPlaced& GrenPlacedAt(float Arg) { return GREN_PLACED[FMath::Clamp((int32)Arg, 0, (int32)UE_ARRAY_COUNT(GREN_PLACED) - 1)]; }
	// L-1-b 폭발 선 자리(L_Expanse) — 수류탄(멈춘 것을 옮김) · 상대 몸 가운데 x · y(바닥은 수류탄 높이 위 150 ~ 아래 300 에서 찾음). 높이 −748 = 바닥 윗면 −838 + 반높이 90(상대 몸 가운데는 GREN_FOE_FLOOR_GAP 2.15 더 높아 선이 250 에 2.15 오른다 — 그 선도 맞음 같음)
	//   0 = 문틀 623(폭발 선은 막고 무기 광선은 지남 — 깃발 MbGB, GDIFF blastOnly) 기둥이 바깥 틀 633 보다 3.2cm 앞으로 나온 층(y −1716.2~−1713) 안 y −1714.6 을 따라 +x — 첫 맞음 623 거리 14.75(x 164.75), 623 을 빼면 기둥 629 거리 50
	//   1 = 같은 y · 같은 거리 250 에서 막힘 없는 대조(x 1150 → 900, 광선 맞음 0 — C:\tmp\mob\lm\l1b\mocl_ray.py)
	//   Drop = 생긴 프레임에 옮겨 떨굴 GREN_PLACED 줄(멈춘 뒤 BlastPass 가 GX · GY · GZ 로 옮긴다 — 멈춘 수류탄은 공중에 그대로)
	//   0 의 수류탄 자리는 문틀 안 순간이동 칸(B_Teleport) 속이라 l1b 판은 장치 겹침을 끈다(DevicesOff — 다리 판 D91 과 같은 끔, 첫 판은 안 꺼 y +1573.88 로 옮겨졌다)
	struct FGrenWallAt { double GX, GY, GZ, FX, FY; int32 Drop; };
	const FGrenWallAt GREN_WALLAT[] = {
		{ 150.0,  -1714.6, -748.0, 400.0, -1714.6, 13 },
		{ 1150.0, -1714.6, -748.0, 900.0, -1714.6, 13 },
	};
	const FGrenWallAt& GrenWallAtOf(float Arg) { return GREN_WALLAT[FMath::Clamp((int32)Arg, 0, (int32)UE_ARRAY_COUNT(GREN_WALLAT) - 1)]; }
	// L-1-b — 점프대 · 순간이동 장치 칸의 겹침을 끈다. 다리 판(MOBridgeSubsystem FindDevices, D91)과 같은 끔 — 우리 게임엔 장치 동작이 없어 정답지도 끈 Lyra 로 뜬다.
	// 청사진 클래스라 부모 줄을 따라 이름으로 가른다. 이미 든 겹침은 칸이 겹침을 다시 셀 때에야 끝나 바로 다시 센다(다리 쪽 주석 그대로)
	void DevicesOff(UWorld* World, const TCHAR* When)
	{
		static const FName LauncherName(TEXT("B_Launcher_C"));
		static const FName TeleportName(TEXT("B_Teleport_C"));
		int32 Found = 0, Teleports = 0, Off = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			int32 Kind = 0;	// 1 = 점프대 · 2 = 순간이동
			for (const UClass* C = It->GetClass(); C && !Kind; C = C->GetSuperClass())
			{
				if (C->GetFName() == LauncherName) Kind = 1;
				else if (C->GetFName() == TeleportName) Kind = 2;
			}
			if (!Kind) continue;
			++Found;
			Teleports += Kind == 2 ? 1 : 0;
			TInlineComponentArray<UPrimitiveComponent*> Prims(*It);
			for (UPrimitiveComponent* P : Prims)
			{
				if (!P->GetGenerateOverlapEvents()) continue;
				P->SetGenerateOverlapEvents(false);
				P->UpdateOverlaps();
				++Off;
			}
		}
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_DEVICES when=%s found=%d teleports=%d shapesOff=%d"), When, Found, Teleports, Off);
	}
	// L-1-b 집(L_Expanse) — 바닥 783 윗면 −841 · −y 200 의 떨굼 자리(GREN_PLACED 13)도 같은 바닥 · 등 뒤 400(상대 · 바닥 741 윗면 −843.7). 폭발 칸 수류탄에서 450 밖(가장 가까운 대조 수류탄에서 내 몸 가운데까지 542)
	constexpr double L1B_HOME_X = 800.0, L1B_HOME_Y = -1300.0, L1B_HOME_FLOOR_HINT = -841.0;
	// L-1-a 정한 자리 — 자리는 L_ShooterGym 충돌 덤프(map_g1a_gym.mocl, C:\tmp\mob\lm\l1a\mocl_list3.py)의 상자로 정했다(바닥 윗면 z 50).
	//   구석 = 지붕(물체 6, x 1000~1500 · y −3000~0 · 윗면 350) 서쪽 면 x 1000 과 둔덕(물체 8, x 500~1000 · y −3000~−1000 · 윗면 250) 북쪽 면 y −1000 이 만나는 안쪽 (1000, −1000).
	//     대각 바깥 800(나지막한 상자 20~27 · 윗면 55~60 위로 날아감) · 둔덕 면을 따라(x 쪽) · 지붕 면을 따라(y 쪽) 셋 — 둘째 면을 잇달아 맞는지(구석 · 한 프레임 여러 번 맞음)
	//   벽 스침 = 지붕 서쪽 면 x 1000 을 따라 −Y 로(오른쪽이 벽) 구석 앞 점을 겨눔. 선 시작이 벽에서 떨어진 만큼 들어오는 각이 커서 서는 x 를 넷으로(미끄러짐 = 거의 스칠 때만, 미결 20)
	//   아래로 = 높은 단(물체 14, x 0~2000 · y 2000~2500 · 윗면 550) 서쪽 끝에서 −X 로 피치 −45 · −60 · −75, 지붕 서쪽 끝에서 −60(최대 속도 2550 자름, 미결 19 — 바닥까지 약 500 · 300)
	//   턱 틈 = 둔덕 윗면이 지붕 서쪽 면과 만나는 곳(윗면 + 벽, 두 면) · 턱(물체 10, 윗면 150)이 둔덕 서쪽 면 x 500 과 만나는 곳
	struct FGrenFixed { double StandX, StandY; bool bAimPoint; double AimX, AimY, AimZ; float Pitch, Yaw; };
	const FGrenFixed GREN_FIXED[] = {
		{ 434.3,  -434.3, true,  978.8, -978.8, 110.0, 0.f,   0.f },	// 0 구석 대각 · 바닥 위 60
		{ 434.3,  -434.3, true,  978.8, -978.8, 200.0, 0.f,   0.f },	// 1 구석 대각 · 바닥 위 150
		{ 434.3,  -434.3, true,  978.8, -978.8, 60.0,  0.f,   0.f },	// 2 구석 대각 · 바닥 위 10
		{ 600.0,  -940.0, true,  978.8, -978.8, 110.0, 0.f,   0.f },	// 3 둔덕 북쪽 면을 따라 구석으로
		{ 940.0,  -300.0, true,  978.8, -978.8, 110.0, 0.f,   0.f },	// 4 지붕 서쪽 면을 따라 구석으로
		{ 960.0,  -100.0, true,  995.0, -960.0, 150.0, 0.f,   0.f },	// 5 벽 스침 — 캡슐 가운데 x 960
		{ 940.0,  -100.0, true,  995.0, -960.0, 150.0, 0.f,   0.f },	// 6
		{ 920.0,  -100.0, true,  995.0, -960.0, 150.0, 0.f,   0.f },	// 7
		{ 900.0,  -100.0, true,  995.0, -960.0, 150.0, 0.f,   0.f },	// 8
		{ 40.0,   2250.0, false, 0.0,    0.0,   0.0,   -60.f, 180.f },	// 9 높은 단에서 아래로
		{ 40.0,   2250.0, false, 0.0,    0.0,   0.0,   -45.f, 180.f },	// 10
		{ 40.0,   2250.0, false, 0.0,    0.0,   0.0,   -75.f, 180.f },	// 11
		{ 1040.0, -500.0, false, 0.0,    0.0,   0.0,   -60.f, 180.f },	// 12 지붕 서쪽 끝에서 아래로
		{ 600.0,  -1500.0, true, 995.0, -1500.0, 255.0, 0.f,  0.f },	// 13 둔덕 윗면 + 지붕 서쪽 면
		{ 600.0,  -1500.0, true, 995.0, -1500.0, 300.0, 0.f,  0.f },	// 14
		{ 100.0,  -2500.0, true, 495.0, -2500.0, 155.0, 0.f,  0.f },	// 15 턱 윗면 + 둔덕 서쪽 면
	};
	const FGrenFixed& GrenFixedAt(float Arg) { return GREN_FIXED[FMath::Clamp((int32)Arg, 0, (int32)UE_ARRAY_COUNT(GREN_FIXED) - 1)]; }
	constexpr int32 GREN_LIFE_FIRST = 32;		// 이 차례부터 죽음 · 부활 길 — 칸 앞뒤로 점수판 줄(MOB_CPROBE_SCORE)
	constexpr float FOE_KILL_DAMAGE = 5000.f;	// 상대가 나를 죽이는 피해(내 최대 체력 1000 보다 커서 한 번에 죽는다)
	const FGrenShot& GrenShotAt(float Arg) { return GREN_SHOTS[FMath::Clamp((int32)Arg, 0, (int32)UE_ARRAY_COUNT(GREN_SHOTS) - 1)]; }
	// 블루프린트 숫자 변수 하나(이름으로 찾음 — B_TeamDeathMatchScoring.TargetScore · Phase_PostGame.NewGameTime) — 없으면 −1
	double ReadNumber(const UObject* O, const TCHAR* Prop)
	{
		const FNumericProperty* N = O ? FindFProperty<FNumericProperty>(O->GetClass(), Prop) : nullptr;
		if (!N) return -1.0;
		const void* Ptr = N->ContainerPtrToValuePtr<void>(O);
		return N->IsFloatingPoint() ? N->GetFloatingPointPropertyValue(Ptr) : (double)N->GetSignedIntPropertyValue(Ptr);
	}
	const FStep GRENADE_SCRIPT[] = {
		{ TEXT("settle"),        1.0f, EAct::Home,         0.f,  false },
		{ TEXT("foe_setup"),     0.5f, EAct::FoeSetup,     0.f,  false },	// 다른 팀 · 체력 1000(숨긴 채)
		{ TEXT("sky_p45"),       5.1f, EAct::GrenThrow,    0.f,  false },
		{ TEXT("sky_p60"),       5.1f, EAct::GrenThrow,    1.f,  false },
		{ TEXT("p0"),            5.1f, EAct::GrenThrow,    2.f,  false },
		{ TEXT("floor_m20"),     5.1f, EAct::GrenThrow,    3.f,  false },
		{ TEXT("floor_m35"),     5.1f, EAct::GrenThrow,    4.f,  false },
		{ TEXT("feet_m60"),      5.1f, EAct::GrenThrow,    5.f,  false },
		{ TEXT("run_p0"),        5.1f, EAct::GrenThrow,    6.f,  false },
		{ TEXT("jump_p0"),       5.1f, EAct::GrenThrow,    7.f,  false },
		{ TEXT("crouch"),        0.8f, EAct::CrouchToggle, 0.f,  false },
		{ TEXT("crouch_p0"),     5.1f, EAct::GrenThrow,    8.f,  false },
		{ TEXT("stand"),         0.8f, EAct::StandUp,      0.f,  false },
		{ TEXT("wall_cam350"),   5.1f, EAct::GrenThrow,    9.f,  false },
		{ TEXT("wall_cam480"),   5.1f, EAct::GrenThrow,    10.f, false },
		{ TEXT("wall_cam510"),   5.1f, EAct::GrenThrow,    11.f, false },
		{ TEXT("wall_cam1000"),  5.1f, EAct::GrenThrow,    12.f, false },
		{ TEXT("wall_front"),    5.1f, EAct::GrenThrow,    13.f, false },
		{ TEXT("wall_back"),     5.1f, EAct::GrenThrow,    14.f, false },
		{ TEXT("slope"),         5.1f, EAct::GrenThrow,    15.f, false },
		{ TEXT("corner"),        5.1f, EAct::GrenThrow,    16.f, false },
		{ TEXT("foe_show"),      0.3f, EAct::FoeShow,      0.f,  false },
		{ TEXT("foe_setup2"),    0.5f, EAct::FoeSetup,     0.f,  false },
		{ TEXT("foe_body"),      5.1f, EAct::GrenThrow,    17.f, false },
		{ TEXT("foe_close"),     5.1f, EAct::GrenThrow,    18.f, false },
		{ TEXT("foe_same_team"), 0.3f, EAct::FoeSameTeam,  0.f,  false },
		{ TEXT("foe_same"),      5.1f, EAct::GrenThrow,    19.f, false },
		// G-0-b 피해 칸은 G-0-a 칸 뒤에 붙인다(앞 칸들의 프레임이 그대로라 g0a 판과 줄째 견줄 수 있다 — g0a4 · g0b1 상대 칸 뺀 17칸 3,062/3,062 같음)
		{ TEXT("blast_same200"),    5.1f, EAct::GrenThrow, 31.f, false },
		{ TEXT("foe_setup3"),    0.5f, EAct::FoeSetup,     0.f,  false },
		{ TEXT("blast_hit45"),      5.1f, EAct::GrenThrow, 20.f, false },
		{ TEXT("blast_hit123"),     5.1f, EAct::GrenThrow, 21.f, false },
		{ TEXT("blast_hit225"),     5.1f, EAct::GrenThrow, 22.f, false },
		{ TEXT("blast_hit449"),     5.1f, EAct::GrenThrow, 23.f, false },
		{ TEXT("blast_hit451"),     5.1f, EAct::GrenThrow, 24.f, false },
		{ TEXT("blast_cap449"),     5.1f, EAct::GrenThrow, 25.f, false },
		{ TEXT("blast_cap451"),     5.1f, EAct::GrenThrow, 26.f, false },
		{ TEXT("blast_wall_ledge"), 5.1f, EAct::GrenThrow, 27.f, false },
		{ TEXT("blast_wall_roof"),  5.1f, EAct::GrenThrow, 28.f, false },
		{ TEXT("blast_stand200"),   5.1f, EAct::GrenThrow, 29.f, false },
		{ TEXT("blast_crouch200"),  5.1f, EAct::GrenThrow, 30.f, false },
		// G-0-b2 죽음 · 부활 길도 뒤에 붙인다. 기다림 칸 길이 = 한도(다시 생기면 그 프레임에 끝). 내가 다시 생긴 뒤 3초 쉼 = 내 부활 무적(2.75초)이 끝난 뒤 던진다
		{ TEXT("g_kill_foe"),       5.1f,  EAct::GrenThrow,      32.f, false },
		{ TEXT("foe_respawn"),      12.0f, EAct::FoeRespawnWait, 0.f,  false },
		{ TEXT("g_immune"),         5.1f,  EAct::GrenThrow,      33.f, false },
		{ TEXT("g_die_after"),      5.1f,  EAct::GrenThrow,      34.f, false },
		{ TEXT("me_respawn"),       12.0f, EAct::MeRespawnWait,  0.f,  false },
		{ TEXT("me_settle"),        3.0f,  EAct::Home,           0.f,  false },
		{ TEXT("g_die_direct"),     5.1f,  EAct::GrenThrow,      35.f, false },
		{ TEXT("me_respawn2"),      12.0f, EAct::MeRespawnWait,  0.f,  false },
		{ TEXT("me_settle2"),       3.0f,  EAct::Home,           0.f,  false },
		{ TEXT("g_self_hurt"),      5.1f,  EAct::GrenThrow,      36.f, false },
		{ TEXT("foe_kills_me"),     0.5f,  EAct::FoeKillsMe,     0.f,  false },
		{ TEXT("me_respawn3"),      12.0f, EAct::MeRespawnWait,  0.f,  false },
		{ TEXT("me_settle3"),       3.0f,  EAct::Home,           0.f,  false },
		{ TEXT("g_self_kill"),      5.1f,  EAct::GrenThrow,      37.f, false },
		{ TEXT("me_respawn4"),      12.0f, EAct::MeRespawnWait,  0.f,  false },
		{ TEXT("me_settle4"),       3.0f,  EAct::Home,           0.f,  false },
		// G-0-c 섞어 누름 — 경기 끝 칸 앞에 붙인다(앞 칸들 프레임 그대로 · 끝 단계가 판을 끝내서 그 뒤엔 못 붙인다).
		// c_spam 21초 = 칸 안 16초까지 매 프레임 Q(생김 넷 · 간격 셋) + 마지막 쿨다운 5초가 다음 칸 누름 전에 끝남. 재장전 · 대시 칸 6초 = Q 가 칸 시작 뒤 0.6~0.7초라(g0c2 41 · 36프레임) 5.1초면 다음 칸 Q 와 쿨다운이 맞닿는다
		{ TEXT("c_spam"),           21.0f, EAct::GrenThrow,      40.f, false },
		{ TEXT("c_q_fire"),         5.1f,  EAct::GrenThrow,      41.f, false },
		{ TEXT("c_fire_q"),         5.1f,  EAct::GrenThrow,      42.f, false },
		{ TEXT("c_ammo"),           0.3f,  EAct::SetAmmo,        1.f,  false },	// 권총 4 · 48(재장전할 거리)
		{ TEXT("c_reload_q"),       6.0f,  EAct::GrenThrow,      43.f, false },
		{ TEXT("c_dash_q"),         6.0f,  EAct::GrenThrow,      44.f, false },
		{ TEXT("c_melee_q"),        5.1f,  EAct::GrenThrow,      45.f, false },
		// G-0-c2 수류탄끼리 · 멈춘 수류탄 밟기 — 칸 시작에 쿨다운 효과를 지우고 던진다(c_melee_q 수류탄이 칸 시작 + 37프레임이라(g0c2 15927 → 15964) 쿨다운 301 이 다음 칸 누름에 1프레임 남는다).
		// c2_drop 5초 = 둘째가 첫째 멈춤 + 3프레임에 생겨 120 뒤 터지고 60 뒤 사라짐(칸 약 260프레임째). c2_walk 4초 = 수류탄이 약 211프레임째 사라진 뒤 칸이 끝나 대조 칸엔 수류탄이 없다
		{ TEXT("c2_drop"),          5.0f,  EAct::GrenThrow,      46.f, false },
		{ TEXT("c2_walk"),          4.0f,  EAct::GrenThrow,      47.f, false },
		{ TEXT("c2_walk_bare"),     2.0f,  EAct::GrenThrow,      48.f, false },
		// G-0-b3 경기 끝은 맨 뒤 — Phase_PostGame 은 NewGameTime(10초)을 기다린 뒤 능력 입력을 막고 다음 틱에 맵을 다시 연다(그래프 원본). 판은 내 입력이 막힌 프레임에 끝낸다(Tick).
		// 끝 단계 시간 = 상대가 나를 죽인 뒤 g_end_inflight 남은 235프레임 + 부활 기다림 72프레임(죽고 307) + g_end_throw 180 + 칸 넘김 1 = 488(g0end1 · 2 실측)
		// → end_block 은 막힘(상대가 나를 죽이고 600프레임 뒤, 실측)을 기다리는 한도 360(안 오면 그대로 끝) — 실측 112프레임째 막혔다
		{ TEXT("end_target"),       0.3f,  EAct::LowerTarget,    0.f,  false },
		{ TEXT("g_end_inflight"),   5.1f,  EAct::GrenThrow,      38.f, false },
		{ TEXT("me_respawn5"),      12.0f, EAct::MeRespawnWait,  0.f,  false },
		{ TEXT("g_end_throw"),      3.0f,  EAct::GrenThrow,      39.f, false },
		{ TEXT("end_block"),        6.0f,  EAct::None,           0.f,  false },
	};
	// L-1-a 장면 각본(-MOBCPGrenSet=l1a) — 칸 길이 · 누름 시각은 G-0 던짐 칸과 같다(5.1초 · 쿨다운 5초가 다음 누름 전에 끝남)
	const FStep GRENADE_L1A_SCRIPT[] = {
		{ TEXT("settle"),           1.0f, EAct::Home,      0.f,  false },
		{ TEXT("foe_setup"),        0.5f, EAct::FoeSetup,  0.f,  false },	// 다른 팀 · 체력 1000(숨긴 채)
		{ TEXT("a_corner_z110"),    5.1f, EAct::GrenThrow, 49.f, false },
		{ TEXT("a_corner_z200"),    5.1f, EAct::GrenThrow, 50.f, false },
		{ TEXT("a_corner_z60"),     5.1f, EAct::GrenThrow, 51.f, false },
		{ TEXT("a_corner_xside"),   5.1f, EAct::GrenThrow, 52.f, false },
		{ TEXT("a_corner_yside"),   5.1f, EAct::GrenThrow, 53.f, false },
		{ TEXT("a_graze_x960"),     5.1f, EAct::GrenThrow, 54.f, false },
		{ TEXT("a_graze_x940"),     5.1f, EAct::GrenThrow, 55.f, false },
		{ TEXT("a_graze_x920"),     5.1f, EAct::GrenThrow, 56.f, false },
		{ TEXT("a_graze_x900"),     5.1f, EAct::GrenThrow, 57.f, false },
		{ TEXT("a_down_m60"),       5.1f, EAct::GrenThrow, 58.f, false },
		{ TEXT("a_down_m45"),       5.1f, EAct::GrenThrow, 59.f, false },
		{ TEXT("a_down_m75"),       5.1f, EAct::GrenThrow, 60.f, false },
		{ TEXT("a_down_roof_m60"),  5.1f, EAct::GrenThrow, 61.f, false },
		{ TEXT("a_step_z255"),      5.1f, EAct::GrenThrow, 62.f, false },
		{ TEXT("a_step_z300"),      5.1f, EAct::GrenThrow, 63.f, false },
		{ TEXT("a_step_ledge"),     5.1f, EAct::GrenThrow, 64.f, false },
		{ TEXT("p_floor_slide"),    5.1f, EAct::GrenThrow, 65.f, false },
		{ TEXT("p_floor_multi"),    5.1f, EAct::GrenThrow, 66.f, false },
		{ TEXT("p_floorwall_c1"),   5.1f, EAct::GrenThrow, 67.f, false },
		{ TEXT("p_floorwall_c2"),   5.1f, EAct::GrenThrow, 68.f, false },
		{ TEXT("p_wall_slide_g05"), 5.1f, EAct::GrenThrow, 69.f, false },
		{ TEXT("p_wall_slide_g1"),  5.1f, EAct::GrenThrow, 70.f, false },
		{ TEXT("p_crease_move1"),   5.1f, EAct::GrenThrow, 71.f, false },
		{ TEXT("p_crease_move2"),   5.1f, EAct::GrenThrow, 72.f, false },
		{ TEXT("p_crease_comb1"),   5.1f, EAct::GrenThrow, 73.f, false },
		{ TEXT("p_crease_comb2"),   5.1f, EAct::GrenThrow, 74.f, false },
	};
	// L-1-b 장면 각본(-MOBCPGrenSet=l1b, 맵 L_Expanse) — 칸 길이 · 누름 시각은 G-0 던짐 칸과 같다. 창틀 띠 셋은 상대를 숨긴 채, 폭발 둘은 다시 켜고(G-0 각본 foe_show · foe_setup2 와 같은 차례)
	const FStep GRENADE_L1B_SCRIPT[] = {
		{ TEXT("settle"),           1.0f, EAct::Home,      0.f,  false },
		{ TEXT("foe_setup"),        0.5f, EAct::FoeSetup,  0.f,  false },	// 다른 팀 · 체력 1000(숨긴 채)
		{ TEXT("b_gonly650"),       5.1f, EAct::GrenThrow, 75.f, false },
		{ TEXT("b_gonly650_slant"), 5.1f, EAct::GrenThrow, 76.f, false },
		{ TEXT("b_gonly647"),       5.1f, EAct::GrenThrow, 77.f, false },
		{ TEXT("foe_show"),         0.3f, EAct::FoeShow,   0.f,  false },
		{ TEXT("foe_setup2"),       0.5f, EAct::FoeSetup,  0.f,  false },
		{ TEXT("b_blast623"),       5.1f, EAct::GrenThrow, 78.f, false },
		{ TEXT("b_blast_open"),     5.1f, EAct::GrenThrow, 79.f, false },
	};
	// L-1-c 같은 프레임 누름 각본(-MOBCPGrenSet=l1c) — 칸 길이 · 누름 시각은 G-0 섞어 누름 칸과 같다(5.1초 · Q 는 칸 시작 0.5초 · 쿨다운 5초가 다음 누름 전에 끝남). 재장전 칸 앞엔 탄창을 4 로(c_ammo 와 같음)
	const FStep GRENADE_L1C_SCRIPT[] = {
		{ TEXT("settle"),     1.0f, EAct::Home,      0.f,  false },
		{ TEXT("foe_setup"),  0.5f, EAct::FoeSetup,  0.f,  false },	// 다른 팀 · 체력 1000(숨긴 채)
		{ TEXT("s_ammo1"),    0.3f, EAct::SetAmmo,   1.f,  false },	// 권총 4 · 48(재장전할 거리)
		{ TEXT("s_reload_q"), 5.1f, EAct::GrenThrow, 82.f, false },
		{ TEXT("s_ammo2"),    0.3f, EAct::SetAmmo,   1.f,  false },
		{ TEXT("s_q_reload"), 5.1f, EAct::GrenThrow, 83.f, false },
		{ TEXT("foe_show"),   0.3f, EAct::FoeShow,   0.f,  false },	// 근접 둘은 상대를 보이고(G-0 각본 foe_show 와 같음)
		{ TEXT("s_melee_q"),  5.1f, EAct::GrenThrow, 80.f, false },
		{ TEXT("s_q_melee"),  5.1f, EAct::GrenThrow, 81.f, false },
	};
	// L-1-d ① 상대가 던진 뒤 나감 각본(-MOBCPGrenSet=l1d) — 대조 칸(나가지 않음)에서 나를 죽인 수류탄과 같은 자리 · 같은 던짐에서 나가는 칸만 다르다.
	// 내 체력 FT_MY_HP(피해가 들면 죽는다 — 처치 소식의 공격자를 보려고). 대조 칸 뒤 내가 다시 생길 때까지 기다리고 3초 쉼(부활 무적 166프레임 = 2.77초가 끝남 — L-1-d ② · 상대 쿨다운 5초도 끝남).
	// 봇이 켠 GA_Grenade 는 네 판 다 세계 +X 로 날았다(생긴 회전 0 · 속도 2472, 0, 370) — 상대 몸 요 180(첫 판)도 내 시선 피치 −60(셋째 판)도 따르지 않음.
	// 둘째 판부터 누를 때 상대 조종 회전은 (0, 0)(FTPRESS ctlRot) — 첫 판 값은 기록을 덮어써 없어 무엇을 따르는지는 미상. 내 등 뒤 400 에서 던지면(둘째 판)
	// 내 몸에 바로 맞아 나이 8 에 터져 나가기 전에 끝난다 → 상대를 집 뒤 FT_FOE_BACK · 옆 FT_FOE_SIDE 에 두면(셋째 판) 수류탄이 벽(Cube5)에 맞고
	// (461.07, −1456.40) 에 멈춰 나이 120 에 터진다 → 상대는 그 자리 그대로, 나는 멈춘 자리에서 −x 161 · −y 250(거리 297 — 반경 450 안 · 날아가는 길 y −1456 에서
	// 250 비킴)인 집 + FT_ME_FROM_HOME 으로 옮긴다(넷째 판 — 대조 칸 피해 73.64 로 죽음)
	constexpr double FT_FOE_BACK = -300.0;
	constexpr double FT_FOE_SIDE = 250.0;
	const FVector FT_ME_FROM_HOME(1956.4, 0.0, 0.0);
	constexpr float FT_PRESS_AT = 0.5f;	// 칸 시작 뒤 상대가 던지는 때(G-0 던짐 칸 누름 0.5초와 같음)
	constexpr int32 FT_LEAVE_AFTER = 10;	// 수류탄이 생기고 이만큼 뒤 나감(터짐 120 보다 한참 앞 · 날아가는 중)
	constexpr float FT_MY_HP = 1.f;
	const FStep GRENADE_L1D_SCRIPT[] = {
		{ TEXT("settle"),     1.0f,  EAct::Home,           0.f,   false },
		{ TEXT("foe_show"),   0.3f,  EAct::FoeShow,        0.f,   false },	// 보임(G-0 각본 foe_show 와 같음)
		{ TEXT("foe_setup"),  0.5f,  EAct::FoeSetup,       0.f,   false },	// 다른 팀 · 체력 1000
		{ TEXT("ft_stay"),    5.1f,  EAct::FoeThrow,       0.f,   false },
		{ TEXT("me_respawn"), 12.0f, EAct::MeRespawnWait,  0.f,   false },
		{ TEXT("me_settle"),  3.0f,  EAct::Home,           0.f,   false },
		{ TEXT("ft_leave"),   5.1f,  EAct::FoeThrowLeave,  0.f,   false },
	};
	// L-2-c 가까운 상대 메시 몸 각본(-MOBCPGrenSet=l2c) — G-0 각본 foe_close(자리 18)만 여섯 번. 칸마다 BeginGrenade 가 두 체력을 1000 으로 되돌린다.
	// 상대 서기 자세는 칸마다 애니 시각이 달라 맞은 점이 판마다 다르다(옛 기록 16판 최대 약 8cm) — 여섯 번이 서로 다른 자세 여섯
	const FStep GRENADE_L2C_SCRIPT[] = {
		{ TEXT("settle"),     1.0f, EAct::Home,      0.f,  false },
		{ TEXT("foe_show"),   0.3f, EAct::FoeShow,   0.f,  false },
		{ TEXT("foe_setup"),  0.5f, EAct::FoeSetup,  0.f,  false },	// 다른 팀(맞으면 터짐 — B_Grenade ReceiveHit)
		{ TEXT("foe_close1"), 5.1f, EAct::GrenThrow, 18.f, false },
		{ TEXT("foe_close2"), 5.1f, EAct::GrenThrow, 18.f, false },
		{ TEXT("foe_close3"), 5.1f, EAct::GrenThrow, 18.f, false },
		{ TEXT("foe_close4"), 5.1f, EAct::GrenThrow, 18.f, false },
		{ TEXT("foe_close5"), 5.1f, EAct::GrenThrow, 18.f, false },
		{ TEXT("foe_close6"), 5.1f, EAct::GrenThrow, 18.f, false },
	};
	// L-1-c 대시 같은 프레임 누름 각본(-MOBCPMode=dash -MOBCPDashSet=l1c) — 칸 길이 · 누름 시각은 D-0 대시 중 누름 칸과 같다(2초 · 대시는 칸 시작 DASH_PRESS_AT). Arg 3 = 근접(DASH_ACTIONS)
	const FStep DASH_L1C_SCRIPT[] = {
		{ TEXT("settle"),       1.0f, EAct::Home,        0.f, false },
		{ TEXT("foe_show"),     0.3f, EAct::FoeShow,     0.f, false },	// D-0 각본 melee_dash 앞과 같은 차례(보임 · 다른 팀 · 체력 1000)
		{ TEXT("foe_setup"),    0.5f, EAct::FoeSetup,    0.f, false },
		{ TEXT("s_melee_dash"), 2.0f, EAct::ActDashSame, 3.f, false },
		{ TEXT("s_dash_melee"), 2.0f, EAct::DashActSame, 3.f, false },
		{ TEXT("foe_away"),     1.0f, EAct::FoeAway,     0.f, false },
	};
	const TCHAR* const GREN_TAG = TEXT("InputTag.Weapon.Grenade");	// Q(IMC_ShooterGame) → GA_Grenade, 켜기 = OnInputTriggered
	constexpr float GREN_PRESS_AT = 0.5f;		// 칸 시작에서 Q 까지 — 겨눔(AimAt)이 모이고 벽 거리 맞춤 두 번이 끝난 뒤(제안값)
	constexpr float GREN_ADJUST_AT0 = 0.2f;		// 벽 카메라 칸 — 몸 앞뒤 맞춤 두 번(순간이동 뒤 카메라가 따라온 다음, 제안값)
	constexpr float GREN_ADJUST_AT1 = 0.35f;
	constexpr int32 GREN_RUN_AFTER = 10;		// 달리며 칸 — 누른 뒤 이 프레임까지 더 달린다(켜지는 다음 프레임이 달리는 중이도록)
	constexpr float GREN_JUMP_AT = 0.3f;		// 점프 칸 — 점프 누름(점프 능력 InputTag.Jump)
	constexpr int32 GREN_JUMP_THROW_AFTER = 12;	// 점프 누른 뒤 Q 까지 — 오르는 중(제안값)
	// 섞어 누름 칸(G-0-c) — 매 프레임 Q 를 그치는 칸 안 시각(16초 = 누름 시작 0.5초 + 쿨다운 5초 셋 + 여유 0.5초 → 간격이 301 이어도 생김 넷) · 재장전이 켜지고 Q 까지(제안값)
	constexpr float GREN_SPAM_UNTIL = 16.0f;
	constexpr int32 GREN_RELOAD_Q_AFTER = 10;
	// G-0-c2 — 첫 수류탄이 멈추고 둘째 Q 까지 · 둘째를 놓을 높이(첫째 가운데 위) · 아래로 속도. 걷기 = 수류탄 뒤 거리 · 순간이동 뒤 쉼 · 걷는 프레임(제안값).
	// 집으로 돌아가는 프레임(멈춤 + 1 + 10 + 40 = 멈춤 + 51, g0t1 16610 → 16661)이 터짐(생김 + 120 = 멈춤 + 74) 전이라 폭발에 안 걸린다(집 ↔ 멈춘 자리 약 900cm > 450)
	constexpr int32 TOUCH_DROP_AFTER = 2;
	constexpr double TOUCH_DROP_HEIGHT = 30.0;
	constexpr double TOUCH_DROP_SPEED = 300.0;
	constexpr double TOUCH_WALK_BACK = 150.0;
	constexpr int32 TOUCH_WALK_SETTLE = 10;
	constexpr int32 TOUCH_WALK_FRAMES = 40;
	// 섞어 누름 칸의 다른 능력 — 입력 표식 · 능력 클래스 이름 조각 · 켜진 뒤 Q 까지 프레임(−1 = Q 와 같은 프레임에 누름). 대시 · 근접은 대시 판 「대시 중 누름」과 같은 5
	struct FGrenComboSpec { const TCHAR* OtherTag; const TCHAR* OtherAbility; int32 QAfter; };
	FGrenComboSpec GrenComboSpec(EGrenCombo C)
	{
		switch (C)
		{
		case EGrenCombo::Spam:    return { nullptr, nullptr, -1 };
		case EGrenCombo::QFire:
		case EGrenCombo::FireQ:   return { TEXT("InputTag.Weapon.Fire"),   TEXT("GA_Weapon_Fire"),    -1 };
		case EGrenCombo::ReloadQ: return { TEXT("InputTag.Weapon.Reload"), TEXT("GA_Weapon_Reload_"), GREN_RELOAD_Q_AFTER };
		case EGrenCombo::DashQ:   return { TEXT("InputTag.Ability.Dash"),  TEXT("GA_Hero_Dash"),      DASH_ACTION_AFTER };
		case EGrenCombo::MeleeQ:  return { TEXT("InputTag.Ability.Melee"), TEXT("GA_Melee"),          DASH_ACTION_AFTER };
		case EGrenCombo::MeleeQSame:
		case EGrenCombo::QMeleeSame:  return { TEXT("InputTag.Ability.Melee"), TEXT("GA_Melee"),          -1 };
		case EGrenCombo::ReloadQSame:
		case EGrenCombo::QReloadSame: return { TEXT("InputTag.Weapon.Reload"), TEXT("GA_Weapon_Reload_"), -1 };
		case EGrenCombo::None:    break;
		}
		return { nullptr, nullptr, -1 };
	}
	const TCHAR* GrenComboName(EGrenCombo C)
	{
		switch (C)
		{
		case EGrenCombo::Spam:    return TEXT("spam");
		case EGrenCombo::QFire:   return TEXT("q_fire");
		case EGrenCombo::FireQ:   return TEXT("fire_q");
		case EGrenCombo::ReloadQ: return TEXT("reload_q");
		case EGrenCombo::DashQ:   return TEXT("dash_q");
		case EGrenCombo::MeleeQ:  return TEXT("melee_q");
		case EGrenCombo::MeleeQSame:  return TEXT("melee_q_same");
		case EGrenCombo::QMeleeSame:  return TEXT("q_melee_same");
		case EGrenCombo::ReloadQSame: return TEXT("reload_q_same");
		case EGrenCombo::QReloadSame: return TEXT("q_reload_same");
		case EGrenCombo::None:    break;
		}
		return TEXT("none");
	}
	const TCHAR* GrenTouchName(EGrenTouch T)
	{
		switch (T)
		{
		case EGrenTouch::Drop:     return TEXT("drop");
		case EGrenTouch::Walk:     return TEXT("walk");
		case EGrenTouch::WalkBare: return TEXT("walk_bare");
		case EGrenTouch::None:     break;
		}
		return TEXT("none");
	}
	// GA_Grenade 방향 고르기 · B_Grenade 폭발 값 — grenade-plan.md 「Lyra」 표(원본 기준). 시험판(LogAimTrace · LogBoom)이 같은 식으로 다시 해 본다
	constexpr double GREN_TRACE_START = 450.0;	// Start Trace Offset — 선 시작 = 카메라 + 방향 × 450
	constexpr double GREN_TRACE_RANGE = 3150.0;	// Default Targeting Range — 선 끝 = 카메라 + 방향 × 3150
	constexpr double GREN_CTL_BELOW = 45.0;		// 맞은 거리가 이 아래면 조종 회전 그대로(ga_flow.txt:142-144 Less_DoubleDouble B=45)
	constexpr double GREN_RADIUS = 450.0;		// Radius — 폭발 구 · 등급 나눗수
	constexpr double GREN_WALL_START_GAP = 700.0;	// 벽 카메라 칸 첫 자리 — 캡슐이 벽에서 이만큼. 카메라 거리는 맞춤 두 번이 재서 맞춘다
	constexpr double GREN_WALL_MIN_GAP = 2.0;	// 맞춤이 몸을 벽 안으로 밀지 않게 — 캡슐 겉면 최소 간격(가장 가까워도 wall_front 와 같음)
	constexpr double GREN_SURF_TRACE = 3000.0;	// 구석 찾기 옆 선 길이
	constexpr double GREN_SLOPE_NZ_MIN = 0.2;	// 비스듬한 면 — 맞은 면 법선 Z 가 이 사이(바닥 1 · 벽 0 이 아닌 것, 제안값)
	constexpr double GREN_SLOPE_NZ_MAX = 0.985;	// 약 10도. 처음 0.9 는 이 맵 비탈(g0a1 집 둘레 Cube7 0.966 · Cube6_0 밑면 −0.342 → 윗면 약 0.94)을 다 뺐다
	constexpr double GREN_SLOPE_GRID = 2500.0;	// 비스듬한 면 격자 — 집 둘레 ±이만큼(구석 선 · 방향 선 3150 안)
	constexpr double GREN_SLOPE_STEP = 100.0;	// 격자 간격(51 × 51 선 — 던짐 칸 시작에 한 번)
	constexpr double GREN_SLOPE_DROP_FROM = 1500.0;	// 격자 선 — 집 높이 위 이만큼에서 아래로 두 배 길이
	constexpr double GREN_CORNER_FROM_WALL = 60.0;	// 구석 찾기 — 벽 a 앞 이만큼에서 옆으로 쏜다(벽 겉 굴곡에 안 걸리게, 제안값)
	constexpr double GREN_CORNER_Z = 60.0;		// 구석 겨눔 높이 — 바닥 위(제안값)
	constexpr double GREN_CORNER_STAND = 800.0;	// 구석 칸 던질 자리 — 겨눔 점에서 대각 바깥 이만큼(제안값)
	constexpr double GREN_CORNER_INSET = 30.0;	// 구석이 보이나 — 구석에서 대각 안쪽으로 이만큼 앞 점까지 선이 안 막히면
	constexpr double GREN_AIM_MIN_FOE = 200.0;	// 상대가 이보다 멀면 몸 가운데를 겨눈다(바로 앞 칸은 피치 0 그대로)
	// 피해 칸(G-0-b) — 세움 · 고침 · 마지막 잼 나이(생긴 뒤 프레임). 터짐 120 전, 멈춤(약 46) 뒤. 사이 4프레임 = 옮긴 몸이 다음 프레임 선 추적에 들어간 뒤 잰다(제안값)
	constexpr int32 GREN_BLAST_AT = 96;
	constexpr int32 GREN_BLAST_STEP = 4;
	constexpr double GREN_FOE_FLOOR_GAP = 2.15;	// 서 있는 몸 가운데 = 바닥 + 반높이 + 이것(g0a4 GTRACE to z 142.15 · 바닥 50 · 반높이 90 — 이동이 띄워 두는 틈)
	// 벽 뒤 칸 자리 — L_ShooterGym 충돌 덤프(map_gym.mocl, mocl_list.py) 상자 둘. 연습장엔 얇은 벽이 없어 「턱 위 · 지붕 위 과녁」으로 한다(벽 너머 바닥은 450 안에 없다).
	// 턱 = x −250~750 · y −3000~−2000 · 윗면 150(높이 100)의 북쪽 면 y −2000. 지붕 = x 1000~1500 · y −3000~0 · 윗면 350(높이 300)의 서쪽 면 x 1000(y −1000~0 은 바닥에서 바로 선다).
	// 수류탄은 면 앞 GREN_WALL_BLAST_GAP 바닥, 상대는 면 너머 Arg 윗면. 실행 때 면까지 선 · 윗면 높이를 재서 GWALLSET 줄에 적는다
	constexpr double GREN_LEDGE_X = 250.0, GREN_LEDGE_FACE_Y = -2000.0;
	constexpr double GREN_ROOF_Y = -500.0, GREN_ROOF_FACE_X = 1000.0;
	constexpr double GREN_WALL_BLAST_GAP = 30.0;

	// 선 L → C 가 캡슐(축 C ± (HH − R) · 반지름 R)에 처음 닿는 거리. 축 선분까지 거리 − R 은 선 위에서 볼록이고 C 에서 −R 이라 0 을 한 번만 지난다(이분법)
	double RayCapsuleDist(const FVector& L, const FVector& C, double R, double HH)
	{
		const FVector A0 = C - FVector(0.0, 0.0, HH - R), A1 = C + FVector(0.0, 0.0, HH - R);
		const auto F = [&](double T) { return FMath::PointDistToSegment(L + (C - L) * T, A0, A1) - R; };
		if (F(0.0) <= 0.0) return 0.0;
		double Lo = 0.0, Hi = 1.0;
		for (int32 i = 0; i < 60; ++i)
		{
			const double M = 0.5 * (Lo + Hi);
			(F(M) > 0.0 ? Lo : Hi) = M;
		}
		return Hi * FVector::Dist(L, C);
	}

	const TCHAR* const WEAPON_DEFS[3] = {
		TEXT("/ShooterCore/Weapons/Pistol/WID_Pistol.WID_Pistol_C"),
		TEXT("/ShooterCore/Weapons/Rifle/WID_Rifle.WID_Rifle_C"),
		TEXT("/ShooterCore/Weapons/Shotgun/WID_Shotgun.WID_Shotgun_C"),
	};
	// 가방 아이템 정의 — 다리 LoadWeaponClasses 와 같은 길
	const TCHAR* const ITEM_DEFS[3] = {
		TEXT("/ShooterCore/Weapons/Pistol/ID_Pistol.ID_Pistol_C"),
		TEXT("/ShooterCore/Weapons/Rifle/ID_Rifle.ID_Rifle_C"),
		TEXT("/ShooterCore/Weapons/Shotgun/ID_Shotgun.ID_Shotgun_C"),
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

	// 판 번호 0 = 연습 · 1 = 받침 · 2 = 다리 · 3 = 부딪힘(연습장) · 4 = 무기 받침 · 5 = 대시(연습장) · 6 = 수류탄(연습장) · 7 = 수류탄 L-1-a 장면(연습장) · 8 = 수류탄 L-1-b 장면(L_Expanse) · 9 = 대시 L-1-c 같은 프레임 누름(연습장) · 10 = 수류탄 L-1-c 같은 프레임 누름(연습장) · 11 = 수류탄 L-1-d 상대가 던진 뒤 나감(연습장) · 12 = 수류탄 L-2-c 가까운 상대 메시 몸(연습장)
	const FStep& StepAt(int32 Script, int32 I)
	{
		switch (Script)
		{
		case 1: return PADS_SCRIPT[I];
		case 2: return BRIDGE_SCRIPT[I];
		case 3: return BUMP_SCRIPT[I];
		case 4: return WEAPONS_SCRIPT[I];
		case 5: return DASH_SCRIPT[I];
		case 6: return GRENADE_SCRIPT[I];
		case 7: return GRENADE_L1A_SCRIPT[I];
		case 8: return GRENADE_L1B_SCRIPT[I];
		case 9: return DASH_L1C_SCRIPT[I];
		case 10: return GRENADE_L1C_SCRIPT[I];
		case 11: return GRENADE_L1D_SCRIPT[I];
		case 12: return GRENADE_L2C_SCRIPT[I];
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
		case 4: return UE_ARRAY_COUNT(WEAPONS_SCRIPT);
		case 5: return UE_ARRAY_COUNT(DASH_SCRIPT);
		case 6: return UE_ARRAY_COUNT(GRENADE_SCRIPT);
		case 7: return UE_ARRAY_COUNT(GRENADE_L1A_SCRIPT);
		case 8: return UE_ARRAY_COUNT(GRENADE_L1B_SCRIPT);
		case 9: return UE_ARRAY_COUNT(DASH_L1C_SCRIPT);
		case 10: return UE_ARRAY_COUNT(GRENADE_L1C_SCRIPT);
		case 11: return UE_ARRAY_COUNT(GRENADE_L1D_SCRIPT);
		case 12: return UE_ARRAY_COUNT(GRENADE_L2C_SCRIPT);
		default: return UE_ARRAY_COUNT(GYM_SCRIPT);
		}
	}

	UAbilitySystemComponent* FindASC(const AActor* A)
	{
		return A ? UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(A) : nullptr;
	}

	APawn* PawnOf(const TWeakObjectPtr<AController>& C)
	{
		return C.IsValid() ? C->GetPawn() : nullptr;
	}

	// 봇 머리를 멈춘다 — 나를 쏘거나 움직이지 않게(처음 상대 · 다시 생긴 상대, G-0-b2)
	void StopBrain(const APawn* P)
	{
		AAIController* AI = P ? Cast<AAIController>(P->GetController()) : nullptr;
		if (!AI) return;
		if (AI->BrainComponent) AI->BrainComponent->StopLogic(TEXT("MOBContentProbe"));
		AI->StopMovement();
		AI->ClearFocus(EAIFocusPriority::Gameplay);
		AI->bSetControlRotationFromPawnOrientation = false;
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
	bWeapons = Mode == TEXT("weapons");
	bDash = Mode == TEXT("dash");
	bGrenade = Mode == TEXT("grenade");
	FString GrenSet;
	FParse::Value(Cmd, TEXT("MOBCPGrenSet="), GrenSet);
	bGrenL1A = bGrenade && GrenSet == TEXT("l1a");
	if (bGrenL1A) UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GRENSET set=l1a steps=%d"), (int32)UE_ARRAY_COUNT(GRENADE_L1A_SCRIPT));
	bGrenL1B = bGrenade && GrenSet == TEXT("l1b");
	if (bGrenL1B) UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GRENSET set=l1b steps=%d"), (int32)UE_ARRAY_COUNT(GRENADE_L1B_SCRIPT));
	bGrenL1C = bGrenade && GrenSet == TEXT("l1c");
	if (bGrenL1C) UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GRENSET set=l1c steps=%d"), (int32)UE_ARRAY_COUNT(GRENADE_L1C_SCRIPT));
	bGrenL1D = bGrenade && GrenSet == TEXT("l1d");
	if (bGrenL1D) UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GRENSET set=l1d steps=%d"), (int32)UE_ARRAY_COUNT(GRENADE_L1D_SCRIPT));
	bGrenL2C = bGrenade && GrenSet == TEXT("l2c");
	if (bGrenL2C) UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GRENSET set=l2c steps=%d"), (int32)UE_ARRAY_COUNT(GRENADE_L2C_SCRIPT));
	FString DashSet;
	FParse::Value(Cmd, TEXT("MOBCPDashSet="), DashSet);
	bDashL1C = bDash && DashSet == TEXT("l1c");
	if (bDashL1C) UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_DASHSET set=l1c steps=%d"), (int32)UE_ARRAY_COUNT(DASH_L1C_SCRIPT));
	bForceQuinn = FParse::Param(Cmd, TEXT("MOBCPQuinn"));
	StartRealAt = FPlatformTime::Seconds();
	GAreScreenMessagesEnabled = false;
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_INIT world=%s tag=%s mode=%s"), *GetNameSafe(GetWorld()), *Tag,
		bBridge ? TEXT("bridge") : (bPads ? TEXT("pads") : (bBump ? TEXT("bump") : (bWeapons ? TEXT("weapons") : (bDash ? TEXT("dash") : (bGrenade ? TEXT("grenade") : TEXT("gym")))))));
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
	// 수류탄끼리 · 밟기 칸(G-0-c2)은 이 알림을 내 몸에 걸어 막힘 수 · 그중 수류탄인 수를 센다
	if (bTouchWatch)
	{
		++TouchPawnHits;
		if (IsGrenadeActor(OtherActor)) ++TouchPawnGrenHits;
	}
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
	// 무기 판은 기다리는 동안(처음 생김 · 경기 시작 초기화)과 캐릭터가 없는 동안(죽음 ~ 부활)도 가방 · 받침을 본다
	if (bWeapons) WatchWeapons();
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
	// 다리 판은 봇에게 죽어 다시 생기는 것도 지금 클라의 모습이라 적기만 하고 이어 간다. 무기 판은 죽음 뒤 가방을 보려고 스스로 죽으니 이어 간다.
	// 수류탄 판도 이어 간다(던진 사람이 죽은 뒤 터짐 · 부활 뒤 쿨다운 — 묶음 11)
	if (Me() != ClearPawn.Get())
	{
		UE_LOG(LogMOBCProbe, Warning, TEXT("MOB_CPROBE_PAWN_CHANGED step=%d from=%s to=%s t=%.4f"), Step, *GetNameSafe(ClearPawn.Get()), *GetNameSafe(Me()), GetWorld()->GetTimeSeconds());
		if (!bBridge && !bWeapons && !bGrenade)
		{
			Finish(false);
			return;
		}
		ClearPawn = Me();
		// 수류탄 판은 몸이 없는 프레임(죽음 ~ 부활)도 센다 — 수류탄 나이 · 칸 시간이 프레임마다 가야 터짐 프레임이 맞다(G-0-b2)
		if (!ClearPawn.IsValid() && !bGrenade) return;
	}

	if (bBump && !bHitsBound && Me() && Foe.IsValid())
	{
		Me()->OnActorHit.AddDynamic(this, &UMOBContentProbeSubsystem::OnPawnHit);
		Foe->OnActorHit.AddDynamic(this, &UMOBContentProbeSubsystem::OnPawnHit);
		bHitsBound = true;
	}
	if (bGrenade)
	{
		WatchGrenades();
		WatchLife();
		WatchPhase();
	}
	DuringStep();
	if (bGrenade) FlushShots(StepAt(ScriptIdx(), Step).Name, GetWorld()->GetTimeSeconds());
	WriteRow();
	// G-0-b3 — 끝 단계에서 내 능력 입력이 막힌 프레임에 끝낸다. 다음 틱에 Lyra 가 맵을 다시 연다(PlayNextGame) — 그 도중에 끄다 무너진 적이 있다(파일럿 c1, ULyraExperienceManagerComponent::EndPlay)
	if (bPostBlocked)
	{
		EndStep();
		Finish(true);
		return;
	}
	StepTime += DeltaTime;
	++Frame;
	if (bStepDone || StepTime + KINDA_SMALL_NUMBER >= StepAt(ScriptIdx(), Step).Seconds)
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
	// 기다리는 동안 연습장 봇에게 죽으면(g0a2 — 준비 때 hp 0 · 죽은 몸은 충돌이 꺼져 집 고르기 선이 하나도 안 맞고 첫 던짐이 막혔다) 그 몸으로 시작하지 않는다.
	// 새 몸이 생기면 처음부터 다시 센다. 다리 판은 서버가 체력을 정하므로 뺀다
	const bool bDeadBlocks = !bBridge && ASC && HealthOf(Ch) <= 0.f;
	if (!ASC || bImmuneBlocks || bDeadBlocks || Ch != ClearPawn.Get())
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

	// 무기 판도 받침을 그대로 둔다(아래 연습 판 갈래는 받침을 끈다)
	if (bPads || bBridge || bWeapons)
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
		StopBrain(Foe.Get());
		GoHome();
		PlaceFoe(-400.0, true);
		// 대시 판 — 상대는 대시 길(뒤 대시가 등 뒤 400 을 지난다)을 막지 않게 숨기고, 집 둘레에서 여덟 방향이 빈 자리를 집으로 고른다(MOB_CPROBE_DASHHOME · CLEAR 줄)
		// 수류탄 판도 같은 집 · 같은 숨김(사람 칸에서만 다시 켬)
		if (bDash || bGrenade)
		{
			SetFoeActive(false);
			if (bGrenL1B)
			{
				// L-1-b — 연습장 집(GYM_HOME · 고리 고르기)은 연습장 바닥 z 50 에 맞춘 자리라 L_Expanse 에선 정한 집에 선다. 바닥은 힌트 높이 둘레에서만(위 지붕을 안 잡게)
				const UCapsuleComponent* Cap = Ch->GetCapsuleComponent();
				FCollisionQueryParams Params(SCENE_QUERY_STAT(MOBCProbeHome), false, Ch);
				if (Foe.IsValid()) Params.AddIgnoredActor(Foe.Get());
				const FCollisionResponseParams Response(Cap->GetCollisionResponseToChannels());
				const FVector XY(L1B_HOME_X, L1B_HOME_Y, L1B_HOME_FLOOR_HINT);
				FHitResult Floor;
				const bool bFloor = World->LineTraceSingleByChannel(Floor, XY + FVector(0.0, 0.0, 150.0), XY - FVector(0.0, 0.0, 300.0), Cap->GetCollisionObjectType(), Params, Response);
				Home = FVector(XY.X, XY.Y, (bFloor ? Floor.ImpactPoint.Z : XY.Z) + Cap->GetScaledCapsuleHalfHeight() + 3.0);
				UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_L1BHOME floor=%d floorActor=%s clear=%d home=%s"), bFloor ? 1 : 0, bFloor ? *GetNameSafe(Floor.GetActor()) : TEXT("-"),
					ClearAround(Home, false) ? 1 : 0, *Home.ToString());
				ClearAround(Home, true);
				DevicesOff(World, TEXT("prepare"));
			}
			else
			{
				PickDashHome();
			}
			GoHome();
			PlaceFoe(-400.0, true);
		}
	}

	Csv += TEXT("frame,t,step,ev,px,py,pz,vx,vy,vz,speed2d,maxSpeed,crouched,capHH,eyeH,camW,camAds,fov,montage,montagePos,meleeActive,adsActive,")
		TEXT("foeX,foeY,foeYaw,foeHp,foeMontage,foeMontagePos,hp,padBigAvail,padBigPct,padSmallAvail,padSmallPct,")
		TEXT("handX,handY,handZ,yaw,foeZ,foeHH,walk,mode,foeWalk,foeVx,foeVy,camZ,footZ");
	// 대시 판만 칸을 더한다(다른 판 기록은 바이트째 그대로) — 능력 켜짐 넷 · 쿨다운 남은 초 · 든 무기 탄창 · 조종 피치 · 요 ·
	// 마지막 이동 입력(이번 프레임 이동이 가져간 것 = 대시가 방향으로 쓰는 값) · 이 프레임에 넣은 이동 입력(다음 프레임 이동이 가져간다)
	if (bDash) Csv += TEXT(",dashActive,fireActive,reloadActive,jumpActive,dashCooldown,mag,ctlPitch,ctlYaw,lastInX,lastInY,inX,inY");
	// 수류탄 판만 — 살아 있는 수류탄 수 · 쿨다운 남은 초 · 카메라 자리 · 회전 · camBack(카메라 → 몸을 시선 방향으로 잰 거리 = 사격 시작점까지, 다리 TargetingStart 식) ·
	// 조종 회전 · 능력 켜짐 넷. 수류탄마다 자리 · 속도 · 회전은 따로 cprobe_<tag>_gren.csv(WatchGrenades)
	if (bGrenade)
	{
		Csv += TEXT(",gLive,gCooldown,camX,camY,camZ,camPitch,camYaw,camBack,ctlPitch,ctlYaw,grenadeActive,fireActive,reloadActive,dashActive");
		GrenCsv = TEXT("frame,name,age,x,y,z,vx,vy,vz,pitch,yaw,roll,hidden,pmActive,sim,sliding\n");
		SpawnedHandle = World->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateUObject(this, &UMOBContentProbeSubsystem::OnActorSpawned));
		// G-0-b2 — 소식 셋을 듣고(MOB_CPROBE_GMSG) 상대 컨트롤러를 붙잡는다(다시 생긴 상대 몸을 찾는다)
		for (const TCHAR* Chan : { TEXT("Lyra.Elimination.Message"), TEXT("Lyra.Damage.Message"), TEXT("Lyra.Assist.Message") })
		{
			VerbHandles.Add(UGameplayMessageSubsystem::Get(this).RegisterListener(FGameplayTag::RequestGameplayTag(Chan), this, &UMOBContentProbeSubsystem::OnVerbMsg));
		}
		FoeCtl = Foe.IsValid() ? Foe->GetController() : nullptr;
		WatchEffects(MyPSASC(), 0);	// L-1-d ② — 부활 무적이 걸린 · 풀린 프레임을 효과 알림으로(무적 표식을 매 프레임 보는 생사 줄과 따로)
		WatchEffects(FoeCtl.IsValid() ? FindASC(FoeCtl->PlayerState) : nullptr, 1);
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GPARAMS aimChannel=%d boomChannel=%d boomObject=%d weaponChannel=%d weaponCapsuleChannel=%d shots=%d psAscSame=%d"),
			(int32)UEngineTypes::ConvertToCollisionChannel(TraceTypeQuery4), (int32)UEngineTypes::ConvertToCollisionChannel(TraceTypeQuery5),
			(int32)UEngineTypes::ConvertToCollisionChannel(ObjectTypeQuery3), (int32)Lyra_TraceChannel_Weapon, (int32)Lyra_TraceChannel_Weapon_Capsule, (int32)UE_ARRAY_COUNT(GREN_SHOTS),
			(MyPSASC() && MyPSASC() == FindASC(Ch)) ? 1 : 0);
	}
	Csv += TEXT("\n");
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
	bStepDone = false;
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
	// 대시 판 — 칸마다 다시 센다. 대시 칸은 순간이동으로 시작하니 앞 칸 이동 입력을 비운다(근접 한 번 칸과 같은 까닭)
	if (bDash)
	{
		if (Ch && IsDashAct(S.Act)) Ch->ConsumeMovementInputVector();
		DashIn = FVector::ZeroVector;
		FrameIn = FVector::ZeroVector;
		bDashWasActive = IsAbilityActive(TEXT("GA_Hero_Dash"));
		DashPressFrame = DashStartFrame = DashEndFrame = -1;
		DashStarts.Reset();
		DashMontageSeen.Reset();
		DashCooldownAtPress = -1.f;
		DashCrouchedAtPress = -1;
		DashAtPress = FVector::ZeroVector;
		DashMaxSpeed2d = DashMaxVz = DashMinVz = 0.0;
		DashJumpPressFrame = ActionPressFrame = ActionStartFrame = ActionEndFrame = ActionRestartFrame = ActionAfterDash = -1;
		ActionDuringDash = 0;
		bActionHeld = false;
		DashMeleePressFrame = DashMeleeStartFrame = -1;
		DashFoeHpAtStep = HealthOf(Foe.Get());
		DashMagAtBegin = ActiveMagazine();
	}

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
	case EAct::WeaponPadOn:
		StandOn(WEAPON_PAD_SPOTS[FMath::Clamp((int32)S.Arg, 0, (int32)UE_ARRAY_COUNT(WEAPON_PAD_SPOTS) - 1)]);
		break;
	case EAct::SelectSlot:
	{
		// 키 1 · 2 · 3 의 결과를 직접 낸다 — 영웅 그래프 ChangeQuickbarSlot 은 그 칸에 아이템이 있을 때만 바꾸기 신호(게임플레이 이벤트, 크기 = 칸 번호)를 보낸다
		// (C:\tmp\mob\pickup0\hero_flow.txt:4-32, 원본 기준). 신호를 받는 쪽의 다른 조건은 여기서 안 거친다 — 이 판은 받침 규칙만 본다
		APlayerController* PC = GetWorld()->GetFirstPlayerController();
		ULyraQuickBarComponent* Bar = PC ? PC->FindComponentByClass<ULyraQuickBarComponent>() : nullptr;
		const int32 Want = (int32)S.Arg;
		const bool bHas = Bar && Bar->GetSlots().IsValidIndex(Want) && Bar->GetSlots()[Want] != nullptr;
		if (bHas) Bar->SetActiveSlotIndex(Want);
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_SLOT step=%s want=%d has=%d active=%d"), S.Name, Want, bHas ? 1 : 0, Bar ? Bar->GetActiveSlotIndex() : -1);
		break;
	}
	case EAct::SetAmmo:
	{
		static const FGameplayTag MagTag = FGameplayTag::RequestGameplayTag(TEXT("Lyra.ShooterGame.Weapon.MagazineAmmo"));
		static const FGameplayTag SpareTag = FGameplayTag::RequestGameplayTag(TEXT("Lyra.ShooterGame.Weapon.SpareAmmo"));
		const FAmmoSet& A = AMMO_SETS[FMath::Clamp((int32)S.Arg, 0, (int32)UE_ARRAY_COUNT(AMMO_SETS) - 1)];
		APlayerController* PC = GetWorld()->GetFirstPlayerController();
		ULyraInventoryManagerComponent* Inv = PC ? PC->FindComponentByClass<ULyraInventoryManagerComponent>() : nullptr;
		UClass* Def = LoadClass<ULyraInventoryItemDefinition>(nullptr, ITEM_DEFS[FMath::Clamp(A.Weapon, 0, 2)]);
		ULyraInventoryItemInstance* Item = (Inv && Def) ? Inv->FindFirstItemStackByDefinition(Def) : nullptr;
		const auto SetStack = [Item](const FGameplayTag& T, int32 Want)
		{
			const int32 Have = Item->GetStatTagStackCount(T);
			if (Have < Want) Item->AddStatTagStack(T, Want - Have);
			else if (Have > Want) Item->RemoveStatTagStack(T, Have - Want);
		};
		if (Item)
		{
			SetStack(MagTag, A.Magazine);
			SetStack(SpareTag, A.Spare);
		}
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_SETAMMO step=%s weapon=%d found=%d mag=%d spare=%d"), S.Name, A.Weapon, Item ? 1 : 0,
			Item ? Item->GetStatTagStackCount(MagTag) : -1, Item ? Item->GetStatTagStackCount(SpareTag) : -1);
		break;
	}
	case EAct::ReloadPress:
		// 키 입력과 같은 길(능력 입력 표식). 한 프레임 누르고 뗀다(DuringStep)
		PressTag(TEXT("InputTag.Weapon.Reload"), true);
		ReloadPressFrame = Frame;
		FrameEvents += TEXT("reload_press;");
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_RELOAD_PRESS step=%s frame=%d t=%.4f"), S.Name, Frame, GetWorld()->GetTimeSeconds());
		break;
	case EAct::SelfDestruct:
		if (ULyraHealthComponent* HC = ULyraHealthComponent::FindHealthComponent(Ch)) HC->DamageSelfDestruct();
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_SELFDESTRUCT step=%s frame=%d t=%.4f hp=%.1f"), S.Name, Frame, GetWorld()->GetTimeSeconds(), HealthOf(Ch));
		break;
	// 대시 판(D-0) — 누르기 · 이동 입력은 DuringDash 가 한다. 여기서는 자리 · 방향 · 이동 입력만 정한다
	case EAct::DashMove:
		GoHome();
		DashIn = KeyInput(S.Arg);
		break;
	case EAct::DashTurn:		// 누르기 전엔 앞 — 누르는 프레임에 Arg 각으로 바꾼다(DuringDash)
		GoHome();
		DashIn = FVector::ForwardVector;
		break;
	case EAct::DashNoInput:
		GoHome();
		FaceTo(0.0, S.Arg);
		break;
	case EAct::DashPitch:
		GoHome();
		FaceTo(S.Arg, 0.0);
		DashIn = FVector::ForwardVector;
		break;
	case EAct::DashJumpApex:	// 앞 입력은 대시를 누른 프레임부터(DuringDash)
	case EAct::DashSpam:		// 입력 없이 — 몸 요 0 이라 카메라 앞으로(짐작 1 의 앞 갈래)
	case EAct::MeleeDash:		// 오른쪽 입력은 근접을 누른 프레임부터(DuringDash)
		GoHome();
		if (S.Act == EAct::MeleeDash) PlaceFoe(S.Arg, true);
		break;
	case EAct::DashThen:
	case EAct::HoldThenDash:	// 쥐기는 칸 첫 프레임에(DuringDash)
	case EAct::DashCrouchSame:
		GoHome();
		DashIn = FVector::ForwardVector;
		break;
	case EAct::ActDashSame:
	case EAct::DashActSame:	// L-1-c — 같은 프레임 두 누름의 켜짐 차례를 능력 알림(MOB_CPROBE_GABIL seq)으로 본다. 칸 끝 요약 MOB_CPROBE_DCOMBO(EndDash)
		GoHome();
		DashIn = FVector(0.0, 1.0, 0.0);	// 오른쪽(+Y) — 상대는 앞 X(누르는 프레임에 세움, DuringDash). melee_dash 와 같은 축 가름(근접은 앞 · 대시는 옆)
		ComboOns.Reset();
		ComboFails.Reset();
		bComboWatch = true;
		WatchAbilities();
		break;
	case EAct::ReloadDash:
		GoHome();
		DashIn = FVector::ForwardVector;
		PressTag(TEXT("InputTag.Weapon.Reload"), true);	// 한 프레임 누르고 뗀다(DuringStep 의 ReloadPressFrame 갈래)
		ReloadPressFrame = Frame;
		FrameEvents += TEXT("reload_press;");
		break;
	case EAct::DashWall:
	{
		// 집에서 가장 가까운 벽(네 축 방향 중, ClearAround) — 캡슐이 벽에서 Arg cm 떨어진 자리에 그쪽을 보고 선다.
		// 벽 = 맞은 면이 서 있는 것(법선 Z 가 DASH_WALL_NORMAL_Z 아래). dash2 는 가장 가까운 것을 골라 경사(Cube6_0)를 올랐다(Z +289)
		int32 Best = -1;
		for (int32 i = 0; i < 8; i += 2)
		{
			if (ClearDist[i] < DASH_CLEAR_TRACE && FMath::Abs(ClearNormalZ[i]) < DASH_WALL_NORMAL_Z && (Best < 0 || ClearDist[i] < ClearDist[Best])) Best = i;
		}
		GoHome();
		if (Ch && Best >= 0)
		{
			const FVector Dir = KeyInput(Best * 45.f);
			Ch->SetActorLocation(Home + Dir * FMath::Max(0.0, ClearDist[Best] - S.Arg), false, nullptr, ETeleportType::TeleportPhysics);
			FaceTo(0.0, Best * 45.0);
			DashIn = Dir;
		}
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_DASHWALL dir=%d wallDist=%.1f normalZ=%.3f gap=%.1f at=%s"), Best, Best >= 0 ? ClearDist[Best] : -1.0, Best >= 0 ? ClearNormalZ[Best] : -1.0,
			S.Arg, Ch ? *Ch->GetActorLocation().ToString() : TEXT("-"));
		break;
	}
	case EAct::StandUp:
	{
		// 대시가 일으켜 세웠으면 그대로, 아니면 앉기 키(ToggleCrouch)로 세운다
		ALyraCharacter* L = Cast<ALyraCharacter>(Ch);
		const bool bWas = L && L->bIsCrouched;
		if (bWas) L->ToggleCrouch();
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_STAND_UP step=%s wasCrouched=%d"), S.Name, bWas ? 1 : 0);
		break;
	}
	case EAct::FoeShow:
		SetFoeActive(true);
		break;
	case EAct::FoeAway:
		GoHome();
		PlaceFoe(-400.0, true);
		SetFoeActive(false);
		break;
	case EAct::GrenThrow:
		BeginGrenade();
		break;
	// G-0-b2 — 지금 몸이 아닌 산 몸이 생기면 끝(DuringStep). 지금 몸 = 죽는 중인 몸 · 이미 지워졌으면 없음
	case EAct::FoeRespawnWait:
		WaitFromPawn = PawnOf(FoeCtl);
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_WAIT step=%s frame=%d who=foe from=%s"), S.Name, Frame, *GetNameSafe(WaitFromPawn.Get()));
		break;
	case EAct::MeRespawnWait:
		WaitFromPawn = Ch;
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_WAIT step=%s frame=%d who=me from=%s"), S.Name, Frame, *GetNameSafe(Ch));
		break;
	// 상대가 나를 죽인다(FoeKillMe — 수류탄 칸 안에서도 부른다, G-0-b3)
	case EAct::FoeKillsMe:
		LogScores(TEXT("foe_kills_me_before"));
		FoeKillMe(S.Name);
		break;
	case EAct::FoeThrow:
	case EAct::FoeThrowLeave:
		BeginFoeThrow();
		break;
	case EAct::LowerTarget:
		LowerTargetScore(S.Name);
		break;
	}
}

// 상대가 나를 죽인다 — 상대 ASC 로 Lyra 기본 피해 효과(DamageSelfDestruct 와 같은 효과 · 같은 SetByCaller, LyraHealthComponent.cpp:279-309)를 내게.
// 맥락 = 상대 ASC 의 MakeEffectContext(시킨 쪽 = 상대 선수 상태 · 일으킨 것 = 상대 몸) — 처치 소식의 Instigator 가 상대가 된다
void UMOBContentProbeSubsystem::FoeKillMe(const TCHAR* StepName)
{
	ACharacter* Ch = Me();
	UAbilitySystemComponent* FA = FindASC(Foe.Get());
	UAbilitySystemComponent* MA = FindASC(Ch);
	const TSubclassOf<UGameplayEffect> GE = ULyraGameData::Get().DamageGameplayEffect_SetByCaller.LoadSynchronous();
	bool bApplied = false;
	if (FA && MA && GE)
	{
		const FGameplayEffectSpecHandle Spec = FA->MakeOutgoingSpec(GE, 1.f, FA->MakeEffectContext());
		if (Spec.Data.IsValid())
		{
			Spec.Data->SetSetByCallerMagnitude(LyraGameplayTags::SetByCaller_Damage, FOE_KILL_DAMAGE);
			FA->ApplyGameplayEffectSpecToTarget(*Spec.Data, MA);
			bApplied = true;
		}
	}
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_FOEKILL step=%s frame=%d applied=%d ge=%s foe=%s myHp=%.1f"), StepName, Frame, bApplied ? 1 : 0, *GetNameSafe(GE), *GetNameSafe(Foe.Get()), HealthOf(Ch));
}

// L-1-d ① 상대가 던진다 — 상대를 집 뒤 FT_FOE_BACK · 옆 FT_FOE_SIDE 에 +X 를 보게 세우고 나는 집 + FT_ME_FROM_HOME 으로(수류탄이 멈출 자리 옆), 내 체력을 FT_MY_HP 로. 던짐은 칸 FT_PRESS_AT 에
// 상대 선수 상태 ASC 의 GA_Grenade 를 켠다(봇은 입력 처리가 없어 누름 표식 대신 능력을 바로 켠다 — 켜는 방식 0 = 누른 프레임에 켜짐과 같은 길, g0t2 GABIL_SPEC)
void UMOBContentProbeSubsystem::BeginFoeThrow()
{
	const FStep& S = StepAt(ScriptIdx(), Step);
	ACharacter* Ch = Me();
	GoHome();
	PlaceFoe(FT_FOE_BACK, false, FT_FOE_SIDE);	// 집에 선 나를 기준으로 — 나를 옮기기 전에
	if (Ch)
	{
		Ch->SetActorLocationAndRotation(Home + FT_ME_FROM_HOME, FRotator::ZeroRotator, false, nullptr, ETeleportType::TeleportPhysics);
		if (UCharacterMovementComponent* M = Ch->GetCharacterMovement()) M->StopMovementImmediately();
	}
	SetHealthOf(Ch, 1000.f, FT_MY_HP);
	SetHealthOf(Foe.Get(), 1000.f, 1000.f);
	FTPressFrame = FTSpawnFrame = FTLeaveFrame = FTPsGoneFrame = FTMyDeathFrame = FTAbilOk = -1;
	FTMyHpBegin = HealthOf(Ch);
	FTGrenName.Reset();
	FTFoePS = FoeCtl.IsValid() ? FoeCtl->PlayerState.Get() : nullptr;
	LogScores(S.Name);
}

void UMOBContentProbeSubsystem::DuringFoeThrow()
{
	const FStep& S = StepAt(ScriptIdx(), Step);
	if (FTPressFrame < 0 && StepTime >= FT_PRESS_AT)
	{
		FTPressFrame = Frame;
		UAbilitySystemComponent* FA = FoeCtl.IsValid() ? FindASC(FoeCtl->PlayerState) : nullptr;
		FString Abil = TEXT("-");
		FTAbilOk = 0;
		if (FA)
		{
			for (const FGameplayAbilitySpec& Spec : FA->GetActivatableAbilities())
			{
				if (!Spec.Ability || !Spec.Ability->GetClass()->GetName().StartsWith(TEXT("GA_Grenade"))) continue;
				Abil = Spec.Ability->GetClass()->GetName();
				FTAbilOk = FA->TryActivateAbility(Spec.Handle) ? 1 : 0;
				break;
			}
		}
		const AController* FC = FoeCtl.Get();
		const FRotator CR = FC ? FC->GetControlRotation() : FRotator::ZeroRotator;
		FrameEvents += TEXT("foe_throw;");
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_FTPRESS step=%s frame=%d asc=%d ability=%s ok=%d foe=%s foeLoc=%s ctlRot=%.4f,%.4f"), S.Name, Frame, FA ? 1 : 0, *Abil, FTAbilOk,
			*GetNameSafe(Foe.Get()), Foe.IsValid() ? *Foe->GetActorLocation().ToString() : TEXT("-"), CR.Pitch, CR.Yaw);
	}
	if (FTPressFrame >= 0 && FTSpawnFrame < 0)
	{
		for (const FGrenRec& R : Grens)
		{
			if (R.SpawnFrame < FTPressFrame || !R.A.IsValid() || !Foe.IsValid() || R.A->GetInstigator() != Foe.Get()) continue;
			FTSpawnFrame = R.SpawnFrame;
			FTGrenName = R.Name;
			break;
		}
	}
	// 나감 — 엔진 사람 나감 차례 그대로: 몸 지움(PawnLeavingGame) → 컨트롤러 지움(Controller::Destroyed → CleanupPlayerState 가 선수 상태 · ASC 를 지움)
	if (S.Act == EAct::FoeThrowLeave && FTSpawnFrame >= 0 && FTLeaveFrame < 0 && Frame >= FTSpawnFrame + FT_LEAVE_AFTER)
	{
		FTLeaveFrame = Frame;
		APawn* FP = Foe.Get();
		AController* FC = FoeCtl.Get();
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_FLEAVE step=%s frame=%d pawn=%s ctl=%s ps=%s gren=%s"), S.Name, Frame, *GetNameSafe(FP), *GetNameSafe(FC), *GetNameSafe(FTFoePS.Get()), *FTGrenName);
		if (FP) FP->Destroy();
		if (FC) FC->Destroy();
		FrameEvents += TEXT("foe_leave;");
	}
	if (FTLeaveFrame >= 0 && FTPsGoneFrame < 0 && !IsValid(FTFoePS.Get())) FTPsGoneFrame = Frame;
	if (FTMyDeathFrame < 0)
	{
		const ULyraHealthComponent* HC = ULyraHealthComponent::FindHealthComponent(Me());
		if (HC && HC->GetDeathState() != ELyraDeathState::NotDead) FTMyDeathFrame = Frame;
	}
}

// 칸 끝 요약 — 수류탄 터짐 · 사라짐은 그 수류탄 줄(GBOOM inst · instAsc · GGONE), 피해 · 처치 공격자는 소식 줄(GMSG inst · instWho)
void UMOBContentProbeSubsystem::EndFoeThrow()
{
	const FStep& S = StepAt(ScriptIdx(), Step);
	const FGrenRec* R = Grens.FindByPredicate([this](const FGrenRec& G) { return G.Name == FTGrenName && G.SpawnFrame == FTSpawnFrame; });
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_FTHROW step=%s leave=%d press=%d abilOk=%d spawn=%d gren=%s leaveAt=%d psGone=%d boom=%d gone=%d myHp=%.3f>%.3f myDeath=%d foe=%s"),
		S.Name, S.Act == EAct::FoeThrowLeave ? 1 : 0, FTPressFrame, FTAbilOk, FTSpawnFrame, FTGrenName.IsEmpty() ? TEXT("-") : *FTGrenName, FTLeaveFrame, FTPsGoneFrame,
		R ? R->BoomFrame : -1, R ? (R->bGone ? 1 : 0) : -1, FTMyHpBegin, HealthOf(Me()), FTMyDeathFrame, *GetNameSafe(Foe.Get()));
	LogScores(S.Name);
}

// L-1-d ② 효과 알림 붙이기 — 지속 효과(시간 있음 · 끝없음)만 걸림 알림이 온다(즉시 효과 = 피해는 안 옴). 무적 표식은 효과가 아닌 길로 붙어도 수 바뀜으로 잡는다
void UMOBContentProbeSubsystem::WatchEffects(UAbilitySystemComponent* A, int32 Who)
{
	static const FGameplayTag Immune = FGameplayTag::RequestGameplayTag(TEXT("Gameplay.DamageImmunity"));
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GEWATCH who=%s asc=%s owner=%s immuneNow=%d"), Who == 0 ? TEXT("me") : TEXT("foe"), *GetNameSafe(A), *GetNameSafe(A ? A->GetOwner() : nullptr),
		(A && A->HasMatchingGameplayTag(Immune)) ? 1 : 0);
	if (!A || Who < 0 || Who > 1) return;
	GEAsc[Who] = A;
	GEAddH[Who] = A->OnActiveGameplayEffectAddedDelegateToSelf.AddUObject(this, &UMOBContentProbeSubsystem::OnGEAdded, Who);
	GERemH[Who] = A->OnAnyGameplayEffectRemovedDelegate().AddUObject(this, &UMOBContentProbeSubsystem::OnGERemoved, Who);
	GETagH[Who] = A->RegisterGameplayTagEvent(Immune, EGameplayTagEventType::NewOrRemoved).AddUObject(this, &UMOBContentProbeSubsystem::OnImmuneTag, Who);
}

// frame = 이 기록의 프레임 번호(알림이 온 때 — 생사 줄과 같은 셈), t = 세계 시각. 풀림 줄 held = 풀린 시각 − 걸린 시각(StartWorldTime)
void UMOBContentProbeSubsystem::OnGEAdded(UAbilitySystemComponent* Target, const FGameplayEffectSpec& Spec, FActiveGameplayEffectHandle Handle, int32 Who)
{
	FGameplayTagContainer Tags;
	Spec.GetAllGrantedTags(Tags);
	const FActiveGameplayEffect* AE = Target ? Target->GetActiveGameplayEffect(Handle) : nullptr;
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GEFX frame=%d t=%.6f step=%s who=%s ev=add ge=%s dur=%.6f start=%.6f tags=[%s]"), Frame, GetWorld()->GetTimeSeconds(),
		bReady ? StepAt(ScriptIdx(), Step).Name : TEXT("-"), Who == 0 ? TEXT("me") : TEXT("foe"), Spec.Def ? *Spec.Def->GetClass()->GetName() : TEXT("-"), Spec.GetDuration(),
		AE ? (double)AE->StartWorldTime : -1.0, *Tags.ToStringSimple());
}

void UMOBContentProbeSubsystem::OnGERemoved(const FActiveGameplayEffect& E, int32 Who)
{
	FGameplayTagContainer Tags;
	E.Spec.GetAllGrantedTags(Tags);
	const double Now = GetWorld()->GetTimeSeconds();
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GEFX frame=%d t=%.6f step=%s who=%s ev=rem ge=%s dur=%.6f start=%.6f held=%.6f tags=[%s]"), Frame, Now,
		bReady ? StepAt(ScriptIdx(), Step).Name : TEXT("-"), Who == 0 ? TEXT("me") : TEXT("foe"), E.Spec.Def ? *E.Spec.Def->GetClass()->GetName() : TEXT("-"), E.GetDuration(),
		(double)E.StartWorldTime, Now - (double)E.StartWorldTime, *Tags.ToStringSimple());
}

void UMOBContentProbeSubsystem::OnImmuneTag(const FGameplayTag ChangedTag, int32 Count, int32 Who)
{
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GEFX frame=%d t=%.6f step=%s who=%s ev=tag tag=%s count=%d"), Frame, GetWorld()->GetTimeSeconds(),
		bReady ? StepAt(ScriptIdx(), Step).Name : TEXT("-"), Who == 0 ? TEXT("me") : TEXT("foe"), *ChangedTag.ToString(), Count);
}

UActorComponent* UMOBContentProbeSubsystem::FindScoring() const
{
	const AGameStateBase* GS = GetWorld() ? GetWorld()->GetGameState() : nullptr;
	if (!GS) return nullptr;
	for (UActorComponent* C : GS->GetComponents())
	{
		if (C && C->GetClass()->GetName().StartsWith(TEXT("B_TeamDeathMatchScoring"))) return C;
	}
	return nullptr;
}

int32 UMOBContentProbeSubsystem::TeamScore(int32 TeamId) const
{
	static const FGameplayTag ElimTag = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.Score.Eliminations"));
	const ULyraTeamSubsystem* Teams = GetWorld() ? GetWorld()->GetSubsystem<ULyraTeamSubsystem>() : nullptr;
	return Teams ? Teams->GetTeamTagStackCount(TeamId, ElimTag) : -1;
}

// G-0-b3(묶음 20) — 목표 점수를 지금 두 팀 점수 중 큰 쪽 + 1 로 낮춘다. 다음에 다른 팀을 처치한 쪽이 이긴다
// (B_TeamDeathMatchScoring.OnEliminationScored → 팀 1 · 2 차례로 GetTeamScore 「Victory」 → HandleVictory → Phase_PostGame, 그래프 원본).
// 변수는 블루프린트 것이라 이름으로 넣고 화면 알림(OnRep_TargetScore)도 부른다 — 다리 ForceCountDown(MOBridgeSubsystem.cpp)과 같은 길
void UMOBContentProbeSubsystem::LowerTargetScore(const TCHAR* StepName)
{
	UActorComponent* Scoring = FindScoring();
	const int32 S1 = TeamScore(1);
	const int32 S2 = TeamScore(2);
	const double Before = ReadNumber(Scoring, TEXT("TargetScore"));
	const int32 Want = FMath::Max(S1, S2) + 1;
	bool bSet = false;
	FNumericProperty* N = Scoring ? FindFProperty<FNumericProperty>(Scoring->GetClass(), TEXT("TargetScore")) : nullptr;
	if (N)
	{
		void* Ptr = N->ContainerPtrToValuePtr<void>(Scoring);
		if (N->IsFloatingPoint()) N->SetFloatingPointPropertyValue(Ptr, (double)Want);
		else N->SetIntPropertyValue(Ptr, (int64)Want);
		if (UFunction* F = Scoring->GetClass()->FindFunctionByName(TEXT("OnRep_TargetScore"))) Scoring->ProcessEvent(F, nullptr);
		bSet = true;
	}
	// 끝 단계 길이 — Phase_PostGame 은 NewGameTime 을 기다린 뒤 능력 입력을 막고 맵을 다시 연다(그래프 원본)
	const UClass* Post = LoadClass<UObject>(nullptr, TEXT("/ShooterCore/Experiences/Phases/Phase_PostGame.Phase_PostGame_C"));
	const double NewGameTime = Post ? ReadNumber(Post->GetDefaultObject(), TEXT("NewGameTime")) : -1.0;
	const ULyraTeamSubsystem* Teams = GetWorld()->GetSubsystem<ULyraTeamSubsystem>();
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const int32 MyTeam = (Teams && PC) ? Teams->FindTeamFromObject(PC) : -1;
	const int32 FoeTeam = (Teams && FoeCtl.IsValid()) ? Teams->FindTeamFromObject(FoeCtl.Get()) : -1;
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_TARGET step=%s frame=%d found=%d set=%d before=%.0f after=%.0f score1=%d score2=%d myTeam=%d foeTeam=%d countDown=%.0f newGameTime=%.3f"),
		StepName, Frame, Scoring ? 1 : 0, bSet ? 1 : 0, Before, ReadNumber(Scoring, TEXT("TargetScore")), S1, S2, MyTeam, FoeTeam, ReadNumber(Scoring, TEXT("CountDownTime")), NewGameTime);
}

// G-0-b3 경기 단계 줄 — 단계(진행 · 끝) · 능력 입력 막힘(Gameplay.AbilityInputBlocked = GE_BlockAbilityInput, LyraAbilitySystemComponent.cpp:218) · 팀 점수 · 목표 점수가 바뀐 프레임에 한 줄.
// 막힘은 선수 상태 ASC 로 본다(몸이 없어도 읽힌다). 끝 단계에서 내 입력이 막히면 그 프레임에 판을 끝낸다(Tick).
// 이 도구는 Q 를 ASC 에 바로 누른다(PressTag) — 사람 키가 거기까지 오는지는 화면 입력 모드로 본다(CommonUI 「Menu」면 게임 쪽 키가 막힌다 — 마우스를 잡은 게임 화면이 아니면, CommonUIActionRouterBase.cpp:667 · 1540-1553)
void UMOBContentProbeSubsystem::WatchPhase()
{
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const ULocalPlayer* LP = PC ? PC->GetLocalPlayer() : nullptr;
	const UCommonUIActionRouterBase* Router = LP ? LP->GetSubsystem<UCommonUIActionRouterBase>() : nullptr;
	const TCHAR* InputMode = Router ? LexToString(Router->GetActiveInputMode()) : TEXT("none");
	static const FGameplayTag PlayingTag = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.GamePhase.Playing"));
	static const FGameplayTag PostTag = FGameplayTag::RequestGameplayTag(TEXT("ShooterGame.GamePhase.PostGame"));
	static const FGameplayTag Blocked = FGameplayTag::RequestGameplayTag(TEXT("Gameplay.AbilityInputBlocked"));
	const ULyraGamePhaseSubsystem* Phases = GetWorld()->GetSubsystem<ULyraGamePhaseSubsystem>();
	const UAbilitySystemComponent* MA = MyPSASC();
	const UAbilitySystemComponent* FA = FoeCtl.IsValid() ? FindASC(FoeCtl->PlayerState) : nullptr;
	const bool bPost = Phases && Phases->IsPhaseActive(PostTag);
	const bool bMeBlocked = MA && MA->HasMatchingGameplayTag(Blocked);
	if (bPost && PostGameFrame < 0) PostGameFrame = Frame;
	const TCHAR* StepName = bReady ? StepAt(ScriptIdx(), Step).Name : TEXT("-");
	if (bMeBlocked && PostGameFrame >= 0 && !bPostBlocked)
	{
		bPostBlocked = true;
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_POSTBLOCK frame=%d step=%s stepT=%.4f postFrame=%d sincePost=%d"), Frame, StepName, StepTime, PostGameFrame, Frame - PostGameFrame);
	}
	const FString Sig = FString::Printf(TEXT("playing=%d post=%d meBlock=%d foeBlock=%d score1=%d score2=%d target=%.0f input=%s"), (Phases && Phases->IsPhaseActive(PlayingTag)) ? 1 : 0, bPost ? 1 : 0,
		bMeBlocked ? 1 : 0, (FA && FA->HasMatchingGameplayTag(Blocked)) ? 1 : 0, TeamScore(1), TeamScore(2), ReadNumber(FindScoring(), TEXT("TargetScore")), InputMode);
	if (Sig == LastPhaseSig) return;
	LastPhaseSig = Sig;
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_PHASE frame=%d t=%.4f step=%s stepT=%.4f %s"), Frame, GetWorld()->GetTimeSeconds(), StepName, StepTime, *Sig);
}

void UMOBContentProbeSubsystem::FaceTo(double Pitch, double Yaw) const
{
	ACharacter* Ch = Me();
	if (!Ch) return;
	Ch->SetActorRotation(FRotator(0.0, Yaw, 0.0), ETeleportType::TeleportPhysics);
	if (APlayerController* PC = Cast<APlayerController>(Ch->GetController())) PC->SetControlRotation(FRotator(Pitch, Yaw, 0.0));
}

// 상대 숨김 — 충돌을 끄면 이동이 바닥을 못 찾아 떨어지니 이동도 멈춘다(MOVE_None). 다시 켤 때 걷기로
void UMOBContentProbeSubsystem::SetFoeActive(bool bOn) const
{
	ACharacter* F = Foe.Get();
	if (!F) return;
	UCharacterMovementComponent* M = F->GetCharacterMovement();
	if (!bOn && M) M->DisableMovement();
	F->SetActorHiddenInGame(!bOn);
	F->SetActorEnableCollision(bOn);
	if (bOn && M) M->SetMovementMode(MOVE_Walking);
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_FOE_ACTIVE on=%d foe=%s"), bOn ? 1 : 0, *GetNameSafe(F));
}

float UMOBContentProbeSubsystem::DashCooldownLeft() const
{
	const UAbilitySystemComponent* A = FindASC(Me());
	static const FGameplayTag Cd = FGameplayTag::RequestGameplayTag(TEXT("GameplayCue.Character.Dash.Cooldown"), false);
	if (!A || !Cd.IsValid()) return -1.f;
	float Left = 0.f;
	for (const float L : A->GetActiveEffectsTimeRemaining(FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(FGameplayTagContainer(Cd)))) Left = FMath::Max(Left, L);
	return Left;
}

int32 UMOBContentProbeSubsystem::ActiveMagazine() const
{
	static const FGameplayTag MagTag = FGameplayTag::RequestGameplayTag(TEXT("Lyra.ShooterGame.Weapon.MagazineAmmo"));
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const ULyraQuickBarComponent* Bar = PC ? PC->FindComponentByClass<ULyraQuickBarComponent>() : nullptr;
	const ULyraInventoryItemInstance* Item = Bar ? Bar->GetActiveSlotItem() : nullptr;
	return Item ? Item->GetStatTagStackCount(MagTag) : -1;
}

// 한 자리에서 여덟 방향 — 캡슐을 DASH_CLEAR_LIFT 만큼 띄워 쓸어 벽까지 거리(ClearDist), 25cm 마다 아래로 광선을 쏴 바닥이 끊기는 첫 거리(-1 = 안 끊김).
// 바닥 광선은 걸음 턱(MaxStepHeight)까지 내려간 바닥도 바닥으로 본다. 여덟 방향 모두 DASH_CLEAR_NEED 넘게 비고 그 안에서 바닥이 안 끊기면 참
bool UMOBContentProbeSubsystem::ClearAround(const FVector& Center, bool bLog)
{
	const ACharacter* Ch = Me();
	if (!Ch) return false;
	const UCapsuleComponent* Cap = Ch->GetCapsuleComponent();
	const UCharacterMovementComponent* M = Ch->GetCharacterMovement();
	const double R = Cap->GetScaledCapsuleRadius(), HH = Cap->GetScaledCapsuleHalfHeight();
	const double StepDown = M ? M->MaxStepHeight : 0.0;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(MOBCProbeClear), false, Ch);
	if (Foe.IsValid()) Params.AddIgnoredActor(Foe.Get());
	const FCollisionResponseParams Response(Cap->GetCollisionResponseToChannels());
	const FVector Start = Center + FVector(0.0, 0.0, DASH_CLEAR_LIFT);
	bool bOk = true;
	for (int32 i = 0; i < 8; ++i)
	{
		const double Yaw = i * 45.0;
		const FVector Dir(FMath::Cos(FMath::DegreesToRadians(Yaw)), FMath::Sin(FMath::DegreesToRadians(Yaw)), 0.0);
		FHitResult Hit;
		const bool bHit = GetWorld()->SweepSingleByChannel(Hit, Start, Start + Dir * DASH_CLEAR_TRACE, FQuat::Identity, Cap->GetCollisionObjectType(),
			FCollisionShape::MakeCapsule(R, HH), Params, Response);
		ClearDist[i] = bHit ? Hit.Distance : DASH_CLEAR_TRACE;
		ClearNormalZ[i] = bHit ? Hit.ImpactNormal.Z : 1.0;
		double Gap = -1.0;
		for (double D = 0.0; D <= ClearDist[i]; D += 25.0)
		{
			const FVector P = Center + Dir * D;
			FHitResult FH;
			if (!GetWorld()->LineTraceSingleByChannel(FH, P, P - FVector(0.0, 0.0, HH + StepDown + 5.0), Cap->GetCollisionObjectType(), Params, Response))
			{
				Gap = D;
				break;
			}
		}
		if (Hit.bStartPenetrating || ClearDist[i] < DASH_CLEAR_NEED || (Gap >= 0.0 && Gap <= DASH_CLEAR_NEED)) bOk = false;
		if (bLog)
		{
			UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_CLEAR dir=%d yaw=%.0f wall=%.1f hit=%d normalZ=%.3f startPen=%d actor=%s floorGap=%.1f"), i, Yaw, ClearDist[i], bHit ? 1 : 0,
				ClearNormalZ[i], Hit.bStartPenetrating ? 1 : 0, *GetNameSafe(Hit.GetActor()), Gap);
		}
	}
	return bOk;
}

// 대시 한 번이 누른 자리에서 최대 939cm 를 간다(dash1 실측) — 연습장 집(GYM_HOME)은 뒤 오른쪽 대각이 경사에 걸렸다(dash1 back_right, Z +65 · 속도 2450 → 790).
// 그래서 대시 판은 연습장 집 둘레 고리(DASH_HOME_RING 간격 × DASH_HOME_RINGS, 고리마다 여덟 방향)에서 ClearAround 를 지나는 첫 자리를 집으로 쓴다. 높이는 바닥 + 반높이 + 3(StandOn 과 같음)
void UMOBContentProbeSubsystem::PickDashHome()
{
	const ACharacter* Ch = Me();
	if (!Ch) return;
	const UCapsuleComponent* Cap = Ch->GetCapsuleComponent();
	const double HH = Cap->GetScaledCapsuleHalfHeight();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(MOBCProbeHome), false, Ch);
	if (Foe.IsValid()) Params.AddIgnoredActor(Foe.Get());
	const FCollisionResponseParams Response(Cap->GetCollisionResponseToChannels());
	int32 Tried = 0;
	for (int32 Ring = 0; Ring <= DASH_HOME_RINGS; ++Ring)
	{
		for (int32 i = 0; i < (Ring == 0 ? 1 : 8); ++i)
		{
			const double A = FMath::DegreesToRadians(i * 45.0);
			const FVector XY = GYM_HOME + FVector(FMath::Cos(A), FMath::Sin(A), 0.0) * (Ring * DASH_HOME_RING);
			FHitResult Floor;
			if (!GetWorld()->LineTraceSingleByChannel(Floor, XY + FVector(0.0, 0.0, 200.0), XY - FVector(0.0, 0.0, 600.0), Cap->GetCollisionObjectType(), Params, Response)) continue;
			const FVector C(XY.X, XY.Y, Floor.ImpactPoint.Z + HH + 3.0);
			++Tried;
			if (!ClearAround(C, false)) continue;
			Home = C;
			UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_DASHHOME ok=1 ring=%d dir=%d tried=%d home=%s"), Ring, i, Tried, *Home.ToString());
			ClearAround(Home, true);
			return;
		}
	}
	UE_LOG(LogMOBCProbe, Warning, TEXT("MOB_CPROBE_DASHHOME ok=0 tried=%d home=%s (연습장 집 그대로)"), Tried, *Home.ToString());
	ClearAround(Home, true);
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
	if (ReloadPressFrame >= 0 && Frame > ReloadPressFrame)
	{
		PressTag(TEXT("InputTag.Weapon.Reload"), false);
		ReloadPressFrame = -1;
		FrameEvents += TEXT("reload_release;");
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
	else if (bDash && IsDashAct(S.Act))
	{
		DuringDash();
	}
	else if (bGrenade && S.Act == EAct::GrenThrow)
	{
		DuringGrenade();
	}
	else if (bGrenade && (S.Act == EAct::FoeThrow || S.Act == EAct::FoeThrowLeave))
	{
		DuringFoeThrow();
	}
	else if (S.Act == EAct::FoeRespawnWait || S.Act == EAct::MeRespawnWait)
	{
		// 다시 생김 = 기다림 시작 몸과 다른 몸 · 죽음 단계 0 · 체력이 읽힘(ASC 가 붙음). 상대면 그 몸을 새 상대로 잡고 머리를 멈춘다
		const bool bMe = S.Act == EAct::MeRespawnWait;
		APawn* P = bMe ? Cast<APawn>(Ch) : PawnOf(FoeCtl);
		const ULyraHealthComponent* HC = ULyraHealthComponent::FindHealthComponent(P);
		if (!bStepDone && P && P != WaitFromPawn.Get() && HC && HC->GetDeathState() == ELyraDeathState::NotDead && HealthOf(P) > 0.f)
		{
			if (!bMe)
			{
				Foe = Cast<ACharacter>(P);
				StopBrain(P);
			}
			static const FGameplayTag Immune = FGameplayTag::RequestGameplayTag(TEXT("Gameplay.DamageImmunity"));
			const UAbilitySystemComponent* A = FindASC(P);
			UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_RESPAWN step=%s frame=%d stepT=%.4f who=%s pawn=%s at=%s hp=%.1f immune=%d cd=%.4f"), S.Name, Frame, StepTime, bMe ? TEXT("me") : TEXT("foe"),
				*GetNameSafe(P), *P->GetActorLocation().ToString(), HealthOf(P), (A && A->HasMatchingGameplayTag(Immune)) ? 1 : 0, bMe ? GrenadeCooldownLeft() : -1.f);
			bStepDone = true;
		}
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
	if (bDash && IsDashAct(S.Act))
	{
		EndDash();
		return;
	}
	if (bGrenade && S.Act == EAct::GrenThrow)
	{
		EndGrenade();
		return;
	}
	if (bGrenade && (S.Act == EAct::FoeThrow || S.Act == EAct::FoeThrowLeave))
	{
		EndFoeThrow();
		return;
	}
	if (S.Act == EAct::FoeRespawnWait || S.Act == EAct::MeRespawnWait || S.Act == EAct::FoeKillsMe)
	{
		if (S.Act != EAct::FoeKillsMe && !bStepDone) UE_LOG(LogMOBCProbe, Error, TEXT("MOB_CPROBE_FAIL respawnTimeout step=%s"), S.Name);
		LogScores(S.Name);
		return;
	}
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

// 대시 칸 한 프레임. 이 틱은 그 프레임의 입력 처리(Lyra 능력 켜기 — ALyraPlayerController::PostProcessInput) · 이동 뒤에 돈다.
// 그래서 여기서 누른 대시는 다음 프레임 입력 처리에서 켜지고, 여기서 넣은 이동 입력은 다음 프레임 이동이 가져간다
void UMOBContentProbeSubsystem::DuringDash()
{
	const FStep& S = StepAt(ScriptIdx(), Step);
	ACharacter* Ch = Me();
	if (!Ch) return;
	const UCharacterMovementComponent* M = Ch->GetCharacterMovement();
	const FDashAction* Act = HasDashAction(S.Act) ? &DashActionAt(S.Arg) : nullptr;
	const TCHAR* const DashTag = TEXT("InputTag.Ability.Dash");

	// 켜짐 가장자리
	const bool bActive = IsAbilityActive(TEXT("GA_Hero_Dash"));
	if (bActive && !bDashWasActive)
	{
		DashStarts.Add(Frame);
		if (DashStartFrame < 0) DashStartFrame = Frame;
		FrameEvents += TEXT("dash_start;");
	}
	else if (!bActive && bDashWasActive)
	{
		if (DashEndFrame < 0) DashEndFrame = Frame;	// 첫 대시의 끝(매 프레임 누름 칸도 activeFrames 는 첫 대시)
		FrameEvents += TEXT("dash_end;");
		if (S.Act == EAct::DashSpam) GoHome();	// 다음 대시 길을 비운다(간격만 잰다)
	}
	bDashWasActive = bActive;
	if (DashStartFrame >= 0 && M)
	{
		DashMaxSpeed2d = FMath::Max(DashMaxSpeed2d, M->Velocity.Size2D());
		DashMaxVz = FMath::Max(DashMaxVz, M->Velocity.Z);
		DashMinVz = FMath::Min(DashMinVz, M->Velocity.Z);
	}
	if (bActive)
	{
		const FString MM = MontageOf(Ch);
		if (!MM.IsEmpty() && !DashMontageSeen.Contains(MM)) DashMontageSeen += MM + TEXT(" ");
	}

	// 누를지
	bool bPress = false;
	switch (S.Act)
	{
	case EAct::DashSpam:
		bPress = true;
		break;
	case EAct::DashJumpApex:
		if (DashJumpPressFrame < 0 && StepTime >= DASH_PRESS_AT)
		{
			PressTag(TEXT("InputTag.Jump"), true);	// 점프 능력(GA_Hero_Jump) — 대시가 점프를 막는 갈래와 같은 길
			DashJumpPressFrame = Frame;
			FrameEvents += TEXT("jump_press;");
		}
		else if (DashJumpPressFrame >= 0 && Frame == DashJumpPressFrame + 1)
		{
			PressTag(TEXT("InputTag.Jump"), false);
			FrameEvents += TEXT("jump_release;");
		}
		// 꼭대기 = 떨어지기 시작한 첫 프레임(낙하 중 위 속도 0 이하). 앞 입력은 이 프레임부터 — 대시가 켜질 때 마지막 이동 입력은 아직 0 이라 몸 요 0 의 카메라 앞으로 간다
		bPress = DashPressFrame < 0 && DashJumpPressFrame >= 0 && Frame > DashJumpPressFrame && M && M->IsFalling() && M->Velocity.Z <= 0.0;
		if (bPress) DashIn = FVector::ForwardVector;
		break;
	case EAct::MeleeDash:
		if (DashMeleePressFrame < 0 && StepTime >= MELEE_PRESS_AT)
		{
			DashFoeHpAtStep = HealthOf(Foe.Get());
			PressTag(TEXT("InputTag.Ability.Melee"), true);
			DashMeleePressFrame = Frame;
			DashIn = FVector::RightVector;
			FrameEvents += TEXT("melee_press;");
		}
		else if (DashMeleePressFrame >= 0 && Frame == DashMeleePressFrame + 1)
		{
			PressTag(TEXT("InputTag.Ability.Melee"), false);
			FrameEvents += TEXT("melee_release;");
		}
		if (DashMeleePressFrame >= 0 && DashMeleeStartFrame < 0 && IsAbilityActive(TEXT("GA_Melee")))
		{
			DashMeleeStartFrame = Frame;
			bPress = DashPressFrame < 0;
		}
		break;
	case EAct::DashCrouchSame:
		if (DashPressFrame < 0 && StepTime >= DASH_PRESS_AT)
		{
			if (ALyraCharacter* L = Cast<ALyraCharacter>(Ch)) L->ToggleCrouch();
			FrameEvents += TEXT("crouch_toggle;");
			bPress = true;
		}
		break;
	case EAct::DashTurn:
		bPress = DashPressFrame < 0 && StepTime >= DASH_PRESS_AT;
		if (bPress) DashIn = KeyInput(S.Arg);	// 아래 이동 입력이 이 프레임부터 바뀐 입력을 넣는다 — 켜지는 다음 프레임의 마지막 이동 입력은 아직 앞
		break;
	default:
		bPress = DashPressFrame < 0 && StepTime >= DASH_PRESS_AT;
		break;
	}
	// L-1-c 그 능력 → 대시 — 같은 프레임 대시 누름 앞에 누른다(대시 → 그 능력은 아래 대시 중 누름 갈래가 같은 프레임에 뒤에 누른다). 떼기는 다음 프레임(아래 갈래)
	if ((S.Act == EAct::ActDashSame || S.Act == EAct::DashActSame) && bPress && ActionPressFrame < 0)
		PlaceFoe(L1C_MELEE_FOE, true);	// 누르기 전 오른쪽 걸음만큼 따라가 내 바로 앞에
	if (S.Act == EAct::ActDashSame && bPress && Act && ActionPressFrame < 0)
	{
		PressTag(Act->InputTag, true);
		ActionPressFrame = Frame;
		bActionHeld = Act->bHold;
		FrameEvents += TEXT("act_press;");
	}
	if (bPress)
	{
		PressTag(DashTag, true);
		if (DashPressFrame < 0)
		{
			DashPressFrame = Frame;
			DashAtPress = Ch->GetActorLocation();
			DashCooldownAtPress = DashCooldownLeft();
			DashCrouchedAtPress = Ch->bIsCrouched ? 1 : 0;
		}
		FrameEvents += TEXT("dash_press;");
	}
	else if (S.Act != EAct::DashSpam && DashPressFrame >= 0 && Frame == DashPressFrame + 1)
	{
		PressTag(DashTag, false);
		FrameEvents += TEXT("dash_release;");
	}

	// 이동 입력 — 칸 처음(또는 정한 프레임)부터 누른 뒤 DASH_INPUT_FRAMES 까지. 대각은 앞 · 옆 두 축 그대로(길이 √2, KeyInput)
	FrameIn = FVector::ZeroVector;
	if (!DashIn.IsNearlyZero() && (DashPressFrame < 0 || Frame <= DashPressFrame + DASH_INPUT_FRAMES))
	{
		Ch->AddMovementInput(DashIn, 1.f);
		FrameIn = DashIn;
	}

	// 대시 중 누름 — 켜진 뒤 DASH_ACTION_AFTER 프레임에(쥔 채 대시 칸은 칸 첫 프레임에). 탭은 다음 프레임에 떼고, 쥠은 칸 끝에(EndDash)
	if (Act)
	{
		const bool bPressAct = S.Act == EAct::HoldThenDash ? ActionPressFrame < 0
			: S.Act == EAct::DashActSame ? (ActionPressFrame < 0 && DashPressFrame == Frame)	// L-1-c 대시 → 그 능력 — 대시를 누른 그 프레임, 대시 누름 뒤
			: (ActionPressFrame < 0 && DashStartFrame >= 0 && Frame >= DashStartFrame + DASH_ACTION_AFTER);
		if (bPressAct)
		{
			PressTag(Act->InputTag, true);
			ActionPressFrame = Frame;
			bActionHeld = Act->bHold;
			FrameEvents += TEXT("act_press;");
		}
		else if (!Act->bHold && ActionPressFrame >= 0 && Frame == ActionPressFrame + 1)
		{
			PressTag(Act->InputTag, false);
			FrameEvents += TEXT("act_release;");
		}
		// 그 능력의 켜짐 가장자리 — 처음 켜짐 · 처음 꺼짐 · 다시 켜짐. 대시와 함께 켜진 프레임 수는 끊기 · 막기를 바로 보여 준다(0 이면 대시 중엔 안 켜짐)
		const bool bActOn = ActionPressFrame >= 0 && IsAbilityActive(Act->Ability);
		if (bActOn && ActionStartFrame < 0)
		{
			ActionStartFrame = Frame;
			FrameEvents += TEXT("act_start;");
		}
		else if (!bActOn && ActionStartFrame >= 0 && ActionEndFrame < 0)
		{
			ActionEndFrame = Frame;
			FrameEvents += TEXT("act_end;");
		}
		else if (bActOn && ActionEndFrame >= 0 && ActionRestartFrame < 0)
		{
			ActionRestartFrame = Frame;
			FrameEvents += TEXT("act_restart;");
		}
		if (bActOn && bActive) ++ActionDuringDash;
		if (bActOn && DashEndFrame >= 0 && ActionAfterDash < 0)
		{
			ActionAfterDash = Frame;
			FrameEvents += TEXT("act_after_dash;");
		}
	}
}

// 대시 칸 요약 한 줄 — 프레임 번호는 기록 표 frame 칸. activeFrames = 꺼진 프레임 − 켜진 프레임, gaps = 매 프레임 누름 칸에서 켜진 프레임 사이(짐작 4)
void UMOBContentProbeSubsystem::EndDash()
{
	const FStep& S = StepAt(ScriptIdx(), Step);
	const ACharacter* Ch = Me();
	const FDashAction* Act = HasDashAction(S.Act) ? &DashActionAt(S.Arg) : nullptr;
	if (S.Act == EAct::DashSpam) PressTag(TEXT("InputTag.Ability.Dash"), false);
	if (Act && bActionHeld) PressTag(Act->InputTag, false);
	FString Gaps;
	for (int32 i = 1; i < DashStarts.Num(); ++i) Gaps += FString::Printf(TEXT("%d "), DashStarts[i] - DashStarts[i - 1]);
	const FVector Moved = (Ch && DashPressFrame >= 0) ? Ch->GetActorLocation() - DashAtPress : FVector::ZeroVector;
	const UCharacterMovementComponent* M = Ch ? Ch->GetCharacterMovement() : nullptr;
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_DASH step=%s arg=%.0f in=%.0f,%.0f press=%d start=%d end=%d activeFrames=%d starts=%d gaps=[%s] cdAtPress=%.4f crouchedAtPress=%d crouchedEnd=%d montage=[%s] ")
		TEXT("maxSpeed2d=%.2f maxVz=%.2f minVz=%.2f moved=%.2f,%.2f,%.2f act=%s actPress=%d actStart=%d actEnd=%d actRestart=%d actDuringDash=%d actAfterDash=%d meleePress=%d meleeStart=%d foeDmg=%.2f mag=%d>%d modeEnd=%d"),
		S.Name, S.Arg, DashIn.X, DashIn.Y, DashPressFrame, DashStartFrame, DashEndFrame, (DashStartFrame >= 0 && DashEndFrame >= 0) ? DashEndFrame - DashStartFrame : -1,
		DashStarts.Num(), *Gaps.TrimEnd(), DashCooldownAtPress, DashCrouchedAtPress, Ch && Ch->bIsCrouched ? 1 : 0, *DashMontageSeen.TrimEnd(),
		DashMaxSpeed2d, DashMaxVz, DashMinVz, Moved.X, Moved.Y, Moved.Z, Act ? Act->Name : TEXT("-"), ActionPressFrame, ActionStartFrame, ActionEndFrame, ActionRestartFrame,
		ActionDuringDash, ActionAfterDash, DashMeleePressFrame, DashMeleeStartFrame, (S.Act == EAct::MeleeDash || S.Act == EAct::ActDashSame || S.Act == EAct::DashActSame) ? DashFoeHpAtStep - HealthOf(Foe.Get()) : 0.f, DashMagAtBegin, ActiveMagazine(),
		M ? (int32)M->MovementMode.GetValue() : -1);
	// L-1-c 같은 프레임 칸 — 켜짐(프레임:능력) · 못 켬 까닭별 (수, 첫, 끝). 줄마다의 차례는 MOB_CPROBE_GABIL seq
	if (S.Act == EAct::ActDashSame || S.Act == EAct::DashActSame)
	{
		FString Fails;
		for (const TPair<FString, FIntVector>& KV : ComboFails) Fails += FString::Printf(TEXT("%s:%d:%d-%d "), *KV.Key, KV.Value.X, KV.Value.Y, KV.Value.Z);
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_DCOMBO step=%s act=%s ons=[%s] fails=[%s]"), S.Name, Act ? Act->Name : TEXT("-"), *ComboOns.TrimEnd(), *Fails.TrimEnd());
		bComboWatch = false;
	}
}

void UMOBContentProbeSubsystem::WriteRow()
{
	ACharacter* Ch = Me();
	if (!Ch)
	{
		// 수류탄 판 — 몸이 없는 프레임(죽음 ~ 부활)은 줄을 안 쓴다. 이 프레임 사건은 버리고(로그 줄에 있다) 앞 프레임 체력만 넘긴다(G-0-b2)
		if (bGrenade)
		{
			PrevMyHp = -1.f;
			PrevFoeHp = HealthOf(Foe.Get());
			FrameEvents.Reset();
		}
		return;
	}
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
	// camZ · footZ = 카메라 높이 · 낮은 발(ball_l · ball_r) 높이 — 사람 확인 자동화(human-check-auto-plan.md)의 앉기 카메라 · 몸 높이 정답지. 뼈가 없으면 -1e6
	double FootZ = -1e6;
	if (const USkeletalMeshComponent* Mesh = Ch->GetMesh())
	{
		for (const TCHAR* Bone : { TEXT("ball_l"), TEXT("ball_r") })
		{
			if (Mesh->GetBoneIndex(Bone) == INDEX_NONE) continue;
			const double Z = Mesh->GetBoneLocation(Bone).Z;
			FootZ = FootZ < -1e5 ? Z : FMath::Min(FootZ, Z);
		}
	}
	Csv += FString::Printf(TEXT("%.3f,%.3f,%.3f,%.4f,%.5f,%.3f,%d,%d,%d,%.5f,%.5f,%.3f,%.3f"), Hand.X, Hand.Y, Hand.Z, Ch->GetActorRotation().Yaw,
		F ? F->GetActorLocation().Z : 0.0, F ? F->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 0.f,
		StepAt(ScriptIdx(), Step).bWalk ? 1 : 0, M ? (int32)M->MovementMode.GetValue() : -1,
		FoeWalks(StepAt(ScriptIdx(), Step).Act) ? 1 : 0, FV.X, FV.Y,
		PC && PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation().Z : -1e6, FootZ);
	if (bDash)
	{
		const FVector LastIn = Ch->GetLastMovementInputVector();
		const FRotator CR = PC ? PC->GetControlRotation() : FRotator::ZeroRotator;
		Csv += FString::Printf(TEXT(",%d,%d,%d,%d,%.4f,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f"),
			IsAbilityActive(TEXT("GA_Hero_Dash")) ? 1 : 0, IsAbilityActive(TEXT("GA_Weapon_Fire")) ? 1 : 0, IsAbilityActive(TEXT("GA_Weapon_Reload_")) ? 1 : 0,
			IsAbilityActive(TEXT("GA_Hero_Jump")) ? 1 : 0, DashCooldownLeft(), ActiveMagazine(), FRotator::NormalizeAxis(CR.Pitch), FRotator::NormalizeAxis(CR.Yaw),
			LastIn.X, LastIn.Y, FrameIn.X, FrameIn.Y);
	}
	if (bGrenade)
	{
		const APlayerCameraManager* Cam = PC ? PC->PlayerCameraManager : nullptr;
		const FVector CL = Cam ? Cam->GetCameraLocation() : FVector::ZeroVector;
		const FRotator CR = Cam ? Cam->GetCameraRotation() : FRotator::ZeroRotator;
		const FRotator Ctl = PC ? PC->GetControlRotation() : FRotator::ZeroRotator;
		int32 Live = 0;
		for (const FGrenRec& R : Grens) Live += R.A.IsValid() ? 1 : 0;
		Csv += FString::Printf(TEXT(",%d,%.4f,%.5f,%.5f,%.5f,%.6f,%.6f,%.5f,%.6f,%.6f,%d,%d,%d,%d"), Live, GrenadeCooldownLeft(), CL.X, CL.Y, CL.Z,
			FRotator::NormalizeAxis(CR.Pitch), FRotator::NormalizeAxis(CR.Yaw), (P - CL) | CR.Vector(), FRotator::NormalizeAxis(Ctl.Pitch), FRotator::NormalizeAxis(Ctl.Yaw),
			IsAbilityActive(TEXT("GA_Grenade")) ? 1 : 0, IsAbilityActive(TEXT("GA_Weapon_Fire")) ? 1 : 0, IsAbilityActive(TEXT("GA_Weapon_Reload_")) ? 1 : 0,
			IsAbilityActive(TEXT("GA_Hero_Dash")) ? 1 : 0);
		PrevCamLoc = CL;
		PrevCamRot = CR;
		PrevMeLoc = P;
		PrevMyHp = HealthOf(Ch);
		PrevFoeHp = HealthOf(F);
	}
	Csv += TEXT("\n");
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

// 가방 한 줄 — 빠른 칸(무기:탄창/예비) · 든 칸 · 가방 전체 아이템(빠른 칸 밖에 남은 것도 보려고) · 몸에 찬 장비
FString UMOBContentProbeSubsystem::InventorySig() const
{
	static const FGameplayTag MagTag = FGameplayTag::RequestGameplayTag(TEXT("Lyra.ShooterGame.Weapon.MagazineAmmo"));
	static const FGameplayTag SpareTag = FGameplayTag::RequestGameplayTag(TEXT("Lyra.ShooterGame.Weapon.SpareAmmo"));
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const ULyraQuickBarComponent* Bar = PC ? PC->FindComponentByClass<ULyraQuickBarComponent>() : nullptr;
	const ULyraInventoryManagerComponent* Inv = PC ? PC->FindComponentByClass<ULyraInventoryManagerComponent>() : nullptr;
	const auto Short = [](const ULyraInventoryItemInstance* I)
	{
		if (!I) return FString(TEXT("-"));
		FString N = GetNameSafe(I->GetItemDef().Get());
		N.RemoveFromStart(TEXT("ID_"));
		N.RemoveFromEnd(TEXT("_C"));
		return FString::Printf(TEXT("%s:%d/%d"), *N, I->GetStatTagStackCount(MagTag), I->GetStatTagStackCount(SpareTag));
	};
	FString S = TEXT("slots=[");
	if (Bar)
	{
		const TArray<ULyraInventoryItemInstance*> Slots = Bar->GetSlots();
		for (int32 i = 0; i < Slots.Num(); ++i) S += (i ? TEXT(" ") : TEXT("")) + Short(Slots[i]);
	}
	S += FString::Printf(TEXT("] active=%d items=["), Bar ? Bar->GetActiveSlotIndex() : -9);
	if (Inv)
	{
		const TArray<ULyraInventoryItemInstance*> Items = Inv->GetAllItems();
		for (int32 i = 0; i < Items.Num(); ++i) S += (i ? TEXT(" ") : TEXT("")) + Short(Items[i]);
	}
	S += TEXT("] equip=[");
	const APawn* P = Me();
	if (const ULyraEquipmentManagerComponent* EM = P ? P->FindComponentByClass<ULyraEquipmentManagerComponent>() : nullptr)
	{
		const TArray<ULyraEquipmentInstance*> Eq = EM->GetEquipmentInstancesOfType(ULyraEquipmentInstance::StaticClass());
		for (int32 i = 0; i < Eq.Num(); ++i) S += (i ? TEXT(" ") : TEXT("")) + GetNameSafe(Eq[i] ? Eq[i]->GetClass() : nullptr);
	}
	return S + TEXT("]");
}

void UMOBContentProbeSubsystem::WatchWeapons()
{
	UWorld* World = GetWorld();
	if (!FailMsgHandle.IsValid())
	{
		// 받침 그래프의 「Broadcast Failure Message」(받침에서 아무것도 못 받음 — ws_flow.txt:110-125)가 보내는 채널
		static const FGameplayTag FailChannel = FGameplayTag::RequestGameplayTag(TEXT("Ability.UserFacingSimpleActivateFail.Message"));
		FailMsgHandle = UGameplayMessageSubsystem::Get(this).RegisterListener(FailChannel, this, &UMOBContentProbeSubsystem::OnFailMsg);
	}
	const double Now = World->GetTimeSeconds();
	const TCHAR* StepName = bReady ? StepAt(ScriptIdx(), Step).Name : TEXT("wait");

	const FString Sig = InventorySig();
	if (Sig != LastInvSig)
	{
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_INV frame=%d t=%.4f step=%s stepT=%.4f pawn=%s %s"), Frame, Now, StepName, StepTime, *GetNameSafe(Me()), *Sig);
		LastInvSig = Sig;
		if (bReady) PendingShots.Emplace(Frame + 20, FString::Printf(TEXT("inv%02d_%s"), ++ShotCount, StepName));
	}
	FlushShots(StepName, Now);

	for (TActorIterator<ALyraWeaponSpawner> It(World); It; ++It)
	{
		float Pct = 0.f;
		const int32 Avail = PadAvailable(*It, Pct) ? 1 : 0;
		const FString Name = It->GetName();
		const int32* Last = LastWeaponPadAvail.Find(Name);
		if (!Last || *Last != Avail)
		{
			UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_WPAD frame=%d t=%.4f step=%s stepT=%.4f pad=%s avail=%d pct=%.4f"), Frame, Now, StepName, StepTime, *Name, Avail, Pct);
			LastWeaponPadAvail.Add(Name, Avail);
		}
	}

	// 재장전 — 태그 개수와 켜진 재장전 능력(이름에 Reload). 탄이 들어온 프레임은 위 가방 줄의 탄창 칸이 바뀐 프레임이다
	static const FGameplayTag ReloadTag = FGameplayTag::RequestGameplayTag(TEXT("Event.Movement.Reload"), false);
	const UAbilitySystemComponent* ASC = FindASC(Me());
	const int32 TagN = ASC ? ASC->GetGameplayTagCount(ReloadTag) : -1;
	FString Abil;
	if (ASC)
	{
		for (const FGameplayAbilitySpec& Spec : ASC->GetActivatableAbilities())
		{
			if (Spec.Ability && Spec.IsActive() && Spec.Ability->GetClass()->GetName().Contains(TEXT("Reload"))) Abil += (Abil.IsEmpty() ? TEXT("") : TEXT(" ")) + Spec.Ability->GetClass()->GetName();
		}
	}
	if (TagN != LastReloadTag || Abil != LastReloadAbilities)
	{
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_RELOAD frame=%d t=%.4f step=%s stepT=%.4f tag=%d abilities=[%s]"), Frame, Now, StepName, StepTime, TagN, *Abil);
		LastReloadTag = TagN;
		LastReloadAbilities = Abil;
	}
}

void UMOBContentProbeSubsystem::OnFailMsg(FGameplayTag Channel, const FLyraAbilitySimpleFailureMessage& Msg)
{
	++FailMsgs;
	const TCHAR* StepName = bReady ? StepAt(ScriptIdx(), Step).Name : TEXT("wait");
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_FAILMSG frame=%d t=%.4f step=%s stepT=%.4f n=%d pc=%s tags=%s reason=\"%s\""), Frame, GetWorld()->GetTimeSeconds(),
		StepName, StepTime, FailMsgs, *GetNameSafe(Msg.PlayerController), *Msg.FailureTags.ToStringSimple(), *Msg.UserFacingReason.ToString());
	PendingShots.Emplace(Frame + 10, FString::Printf(TEXT("fail%02d_a_%s"), FailMsgs, StepName));
	PendingShots.Emplace(Frame + 40, FString::Printf(TEXT("fail%02d_b_%s"), FailMsgs, StepName));
}

// 찍을 프레임이 된 사진 하나(UI 포함) — 한 프레임에 한 장
void UMOBContentProbeSubsystem::FlushShots(const TCHAR* StepName, double Now)
{
	for (int32 i = 0; i < PendingShots.Num(); ++i)
	{
		if (Frame < PendingShots[i].Key) continue;
		const FString Path = FPaths::ProjectSavedDir() / TEXT("MOB") / FString::Printf(TEXT("cprobe_%s_%s.png"), *Tag, *PendingShots[i].Value);
		FScreenshotRequest::RequestScreenshot(Path, true, false);
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_SHOT frame=%d t=%.4f step=%s path=%s"), Frame, Now, StepName, *Path);
		PendingShots.RemoveAt(i);
		break;
	}
}

UAbilitySystemComponent* UMOBContentProbeSubsystem::MyPSASC() const
{
	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	return PC ? FindASC(PC->PlayerState) : nullptr;
}

// G-0-b2 생사 줄 — 나(첫 플레이어 몸) · 상대(봇 컨트롤러의 몸)마다 몸 · 죽음 단계(0 산 · 1 죽는 중 · 2 죽음 끝) · 몸에서 ASC 가 찾아지나 · 무적 표식이 바뀐 프레임에 한 줄.
// 내 줄엔 선수 상태 ASC 의 수류탄 쿨다운도(몸이 없어도 읽힌다 — 부활 뒤 이어지나, 묶음 11)
void UMOBContentProbeSubsystem::WatchLife()
{
	static const FGameplayTag Immune = FGameplayTag::RequestGameplayTag(TEXT("Gameplay.DamageImmunity"));
	for (int32 i = 0; i < 2; ++i)
	{
		APawn* P = i == 0 ? Cast<APawn>(Me()) : PawnOf(FoeCtl);
		const ULyraHealthComponent* HC = ULyraHealthComponent::FindHealthComponent(P);
		const UAbilitySystemComponent* A = FindASC(P);
		const FString Sig = FString::Printf(TEXT("pawn=%s death=%d asc=%d immune=%d"), *GetNameSafe(P), HC ? (int32)HC->GetDeathState() : -1, A ? 1 : 0,
			(A && A->HasMatchingGameplayTag(Immune)) ? 1 : 0);
		if (Sig == LastLifeSig[i]) continue;
		LastLifeSig[i] = Sig;
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_LIFE frame=%d t=%.4f step=%s stepT=%.4f who=%s %s hp=%.1f cd=%.4f"), Frame, GetWorld()->GetTimeSeconds(),
			bReady ? StepAt(ScriptIdx(), Step).Name : TEXT("-"), StepTime, i == 0 ? TEXT("me") : TEXT("foe"), *Sig, HealthOf(P), i == 0 ? GrenadeCooldownLeft() : -1.f);
	}
}

// G-0-b2 소식 셋(처치 · 피해 · 도움 — LyraHealthSet.cpp:131-143 · LyraHealthComponent.cpp:170-181 · AssistProcessor.cpp:51-62) 그대로 적는다.
// 처치 아이콘은 처치 소식의 InstigatorTags 에 든 피해 종류 표식으로 고른다(W_EliminationFeedEntryWidget 그래프) — 처치마다 UI 사진 셋
void UMOBContentProbeSubsystem::OnVerbMsg(FGameplayTag Channel, const FLyraVerbMessage& Msg)
{
	static const FGameplayTag Elim = FGameplayTag::RequestGameplayTag(TEXT("Lyra.Elimination.Message"));
	const TCHAR* StepName = bReady ? StepAt(ScriptIdx(), Step).Name : TEXT("-");
	const APlayerState* IPS = ULyraVerbMessageHelpers::GetPlayerStateFromObject(Msg.Instigator.Get());
	const APlayerState* TPS = ULyraVerbMessageHelpers::GetPlayerStateFromObject(Msg.Target.Get());
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const APlayerState* MyPS = PC ? PC->PlayerState.Get() : nullptr;
	const auto WhoPS = [MyPS](const APlayerState* PS) { return !PS ? TEXT("-") : (PS == MyPS ? TEXT("me") : TEXT("foe")); };
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GMSG frame=%d step=%s stepT=%.4f verb=%s inst=%s instWho=%s target=%s targetWho=%s mag=%.4f instTags=[%s] targetTags=[%s]"),
		Frame, StepName, StepTime, *Channel.ToString(), *GetNameSafe(Msg.Instigator.Get()), WhoPS(IPS), *GetNameSafe(Msg.Target.Get()), WhoPS(TPS), Msg.Magnitude,
		*Msg.InstigatorTags.ToStringSimple(), *Msg.TargetTags.ToStringSimple());
	if (Channel == Elim && bReady)
	{
		++ShotCount;
		PendingShots.Emplace(Frame + 10, FString::Printf(TEXT("elim%02d_a_%s"), ShotCount, StepName));
		PendingShots.Emplace(Frame + 40, FString::Printf(TEXT("elim%02d_b_%s"), ShotCount, StepName));
		PendingShots.Emplace(Frame + 90, FString::Printf(TEXT("elim%02d_c_%s"), ShotCount, StepName));
	}
}

void UMOBContentProbeSubsystem::Finish(bool bOk)
{
	bDone = true;
	if (FailMsgHandle.IsValid()) FailMsgHandle.Unregister();
	for (FGameplayMessageListenerHandle& H : VerbHandles) H.Unregister();
	if (SpawnedHandle.IsValid() && GetWorld()) GetWorld()->RemoveOnActorSpawnedHandler(SpawnedHandle);
	for (int32 i = 0; i < 2; ++i)
	{
		static const FGameplayTag Immune = FGameplayTag::RequestGameplayTag(TEXT("Gameplay.DamageImmunity"));
		UAbilitySystemComponent* A = GEAsc[i].Get();
		if (!A) continue;
		A->OnActiveGameplayEffectAddedDelegateToSelf.Remove(GEAddH[i]);
		A->OnAnyGameplayEffectRemovedDelegate().Remove(GERemH[i]);
		A->RegisterGameplayTagEvent(Immune, EGameplayTagEventType::NewOrRemoved).Remove(GETagH[i]);
	}
	if (UAbilitySystemComponent* A = ComboAsc.Get())
	{
		A->AbilityActivatedCallbacks.Remove(AbilOnHandle);
		A->OnAbilityEnded.Remove(AbilEndHandle);
		A->AbilityFailedCallbacks.Remove(AbilFailHandle);
	}
	if (ACharacter* P = TouchHitPawn.Get()) P->OnActorHit.RemoveDynamic(this, &UMOBContentProbeSubsystem::OnPawnHit);
	LogBodies(TEXT("end"));
	if (bGrenade)
	{
		const FString GPath = FPaths::ProjectSavedDir() / TEXT("MOB") / FString::Printf(TEXT("cprobe_%s_gren.csv"), *Tag);
		const bool bGSaved = FFileHelper::SaveStringToFile(GrenCsv, *GPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GDONE saved=%d path=%s grenades=%d"), bGSaved ? 1 : 0, *GPath, Grens.Num());
	}
	const FString Path = FPaths::ProjectSavedDir() / TEXT("MOB") / FString::Printf(TEXT("cprobe_%s.csv"), *Tag);
	const bool bSaved = FFileHelper::SaveStringToFile(Csv, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_DONE ok=%d saved=%d path=%s frames=%d steps=%d realSec=%.1f"),
		bOk ? 1 : 0, bSaved ? 1 : 0, *Path, Frame, FMath::Max(Step, 0), FPlatformTime::Seconds() - StartRealAt);
	FPlatformMisc::RequestExit(false, TEXT("MOBContentProbe"));
}

// 수류탄 판(G-0) — 생긴 수류탄을 붙잡는다. 엔진은 생성이 다 끝난 뒤(BeginPlay 뒤 — 0.16초 타이머 · 던진 사람 무시가 걸린 뒤, World.cpp OnActorFinishedSpawning) 이 알림을 준다.
// 능력이 방향을 고른 그 실행 줄 안이라 지금 카메라 · 조종 회전 · 내 자리 = 능력이 읽은 값이다(짐작 4). 한 줄 앞 기록 값(Prev*)도 대조로 적는다
void UMOBContentProbeSubsystem::OnActorSpawned(AActor* A)
{
	if (!A || !A->GetClass()->GetName().StartsWith(TEXT("B_Grenade"))) return;
	FGrenRec& R = Grens.AddDefaulted_GetRef();
	R.A = A;
	R.Name = A->GetName();
	R.SpawnFrame = Frame;
	++GrenSpawns;
	if (GrenSpawnFrame < 0) GrenSpawnFrame = Frame;
	FrameEvents += TEXT("g_spawn;");
	A->OnActorHit.AddDynamic(this, &UMOBContentProbeSubsystem::OnGrenadeHit);
	UProjectileMovementComponent* PM = A->FindComponentByClass<UProjectileMovementComponent>();
	if (PM)
	{
		PM->OnProjectileBounce.AddDynamic(this, &UMOBContentProbeSubsystem::OnGrenadeBounce);
		PM->OnProjectileStop.AddDynamic(this, &UMOBContentProbeSubsystem::OnGrenadeStop);
	}
	const ACharacter* Ch = Me();
	const APlayerController* PC = Ch ? Cast<APlayerController>(Ch->GetController()) : nullptr;
	const APlayerCameraManager* Cam = PC ? PC->PlayerCameraManager : nullptr;
	const FVector CamLoc = Cam ? Cam->GetCameraLocation() : FVector::ZeroVector;
	const FRotator CamRot = Cam ? Cam->GetCameraRotation() : FRotator::ZeroRotator;
	const FRotator Ctl = PC ? PC->GetControlRotation() : FRotator::ZeroRotator;
	const FVector L = A->GetActorLocation();
	const FRotator Rot = A->GetActorRotation();
	const FVector V = PM ? PM->Velocity : FVector::ZeroVector;
	const FVector MeL = Ch ? Ch->GetActorLocation() : FVector::ZeroVector;
	const UCharacterMovementComponent* M = Ch ? Ch->GetCharacterMovement() : nullptr;
	const FVector MeV = M ? M->Velocity : FVector::ZeroVector;
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GSPAWN frame=%d step=%s name=%s instigator=%s loc=%.10f,%.10f,%.10f rot=%.10f,%.10f,%.10f vel=%.10f,%.10f,%.10f ")
		TEXT("me=%.10f,%.10f,%.10f mePrev=%.10f,%.10f,%.10f meVel=%.6f,%.6f,%.6f meYaw=%.10f crouched=%d mode=%d ")
		TEXT("cam=%.10f,%.10f,%.10f camRot=%.10f,%.10f camPrev=%.10f,%.10f,%.10f camRotPrev=%.10f,%.10f ctl=%.10f,%.10f"),
		Frame, bReady ? StepAt(ScriptIdx(), Step).Name : TEXT("-"), *R.Name, *GetNameSafe(A->GetInstigator()), L.X, L.Y, L.Z, Rot.Pitch, Rot.Yaw, Rot.Roll, V.X, V.Y, V.Z,
		MeL.X, MeL.Y, MeL.Z, PrevMeLoc.X, PrevMeLoc.Y, PrevMeLoc.Z, MeV.X, MeV.Y, MeV.Z, Ch ? Ch->GetActorRotation().Yaw : 0.0, (Ch && Ch->bIsCrouched) ? 1 : 0,
		M ? (int32)M->MovementMode.GetValue() : -1,
		CamLoc.X, CamLoc.Y, CamLoc.Z, CamRot.Pitch, CamRot.Yaw, PrevCamLoc.X, PrevCamLoc.Y, PrevCamLoc.Z, PrevCamRot.Pitch, PrevCamRot.Yaw, Ctl.Pitch, Ctl.Yaw);
	LogAimTrace(TEXT("now"), CamLoc, CamRot, L);
	LogAimTrace(TEXT("prev"), PrevCamLoc, PrevCamRot, L);
}

// GA_Grenade 「Get Spawn Rotation」 그대로 해 본다(ga_flow.txt:95-139) — 카메라 + 방향 × 450 에서 × 3150 까지 무기 채널 선(TraceTypeQuery4 · 복잡 끔 · 내 캐릭터 빼고).
// 맞은 거리 < 45 면 조종 회전(안 맞으면 거리 0 → 이 갈래, 짐작 1), 아니면 놓을 자리에서 맞은 점(안 맞으면 끝점)을 보는 회전(FindLookAtRotation = MakeFromX).
// 첫 속도 = 그 회전으로 돌린 정규화(1, 0, 0.15) × 2500. 실제 생긴 회전 · 속도(GSPAWN)와 견주면 능력이 어느 카메라 값을 썼나가 갈린다
void UMOBContentProbeSubsystem::LogAimTrace(const TCHAR* Which, const FVector& CamLoc, const FRotator& CamRot, const FVector& SpawnLoc) const
{
	ACharacter* Ch = Me();
	const APlayerController* PC = Ch ? Cast<APlayerController>(Ch->GetController()) : nullptr;
	const FVector Dir = CamRot.Vector();
	const FVector Start = CamLoc + Dir * GREN_TRACE_START;
	const FVector End = CamLoc + Dir * GREN_TRACE_RANGE;
	TArray<AActor*> Ignore;
	if (Ch) Ignore.Add(Ch);
	FHitResult Hit;
	const bool bHit = UKismetSystemLibrary::LineTraceSingle(this, Start, End, TraceTypeQuery4, false, Ignore, EDrawDebugTrace::None, Hit, true);
	const bool bCtl = Hit.Distance < GREN_CTL_BELOW;
	const FRotator Ctl = PC ? PC->GetControlRotation() : FRotator::ZeroRotator;
	const FVector Target = bHit ? Hit.Location : End;
	const FRotator Look = FRotationMatrix::MakeFromX(Target - SpawnLoc).Rotator();
	const FRotator Pick = bCtl ? Ctl : Look;
	const FVector V = Pick.Quaternion().RotateVector(FVector(1.0, 0.0, 0.15).GetSafeNormal()) * 2500.0;
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GAIM frame=%d which=%s hit=%d dist=%.6f actor=%s comp=%s hitLoc=%.6f,%.6f,%.6f normal=%.5f,%.5f,%.5f branch=%s pick=%.10f,%.10f,%.10f velPred=%.6f,%.6f,%.6f"),
		Frame, Which, bHit ? 1 : 0, Hit.Distance, *GetNameSafe(Hit.GetActor()), *GetNameSafe(Hit.GetComponent()), Hit.Location.X, Hit.Location.Y, Hit.Location.Z,
		Hit.ImpactNormal.X, Hit.ImpactNormal.Y, Hit.ImpactNormal.Z, bCtl ? TEXT("ctl") : (bHit ? TEXT("hit") : TEXT("end")), Pick.Pitch, Pick.Yaw, Pick.Roll, V.X, V.Y, V.Z);
}

// 수류탄이 막혀 부딪힐 때마다(BP ReceiveHit 과 같은 알림 — 엔진이 그것 다음에 이 대리자를 부른다). vel = 부딪힌 걸음의 처음 속도(튕김 셈 전).
// comp · objType = 맞은 쪽 성분(사람이면 캡슐인지 메시인지 — 짐작 6)
void UMOBContentProbeSubsystem::OnGrenadeHit(AActor* SelfActor, AActor* OtherActor, FVector NormalImpulse, const FHitResult& Hit)
{
	const UProjectileMovementComponent* PM = SelfActor ? SelfActor->FindComponentByClass<UProjectileMovementComponent>() : nullptr;
	const FVector V = PM ? PM->Velocity : FVector::ZeroVector;
	int32 SpawnF = -1;
	for (const FGrenRec& R : Grens)
	{
		if (R.A.Get() == SelfActor) SpawnF = R.SpawnFrame;
	}
	const TCHAR* Who = !OtherActor ? TEXT("-") : (OtherActor == Me() ? TEXT("me") : (OtherActor == Foe.Get() ? TEXT("foe") : (IsGrenadeActor(OtherActor) ? TEXT("gren") : TEXT("world"))));
	if (SpawnF >= 0 && SpawnF == GrenSpawnFrame)
	{
		++GrenHits;
		if (GrenFirstHitFrame < 0)
		{
			GrenFirstHitFrame = Frame;
			GrenFirstHit = FString::Printf(TEXT("%s/%s/%s"), Who, *GetNameSafe(OtherActor), *GetNameSafe(Hit.GetComponent()));
		}
	}
	FrameEvents += FString::Printf(TEXT("g_hit_%s;"), Who);
	const UPrimitiveComponent* OC = Hit.GetComponent();
	const FVector GL = SelfActor ? SelfActor->GetActorLocation() : FVector::ZeroVector;
	const FRotator GR = SelfActor ? SelfActor->GetActorRotation() : FRotator::ZeroRotator;
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GHIT frame=%d step=%s name=%s age=%d who=%s other=%s comp=%s objType=%d bone=%s time=%.7f dist=%.7f pen=%d penDepth=%.6f ")
		TEXT("normal=%.8f,%.8f,%.8f impactN=%.8f,%.8f,%.8f loc=%.8f,%.8f,%.8f impact=%.8f,%.8f,%.8f start=%.8f,%.8f,%.8f end=%.8f,%.8f,%.8f vel=%.6f,%.6f,%.6f grenLoc=%.8f,%.8f,%.8f grenRot=%.8f,%.8f,%.8f"),
		Frame, bReady ? StepAt(ScriptIdx(), Step).Name : TEXT("-"), *GetNameSafe(SelfActor), SpawnF >= 0 ? Frame - SpawnF : -1, Who, *GetNameSafe(OtherActor), *GetNameSafe(OC),
		OC ? (int32)OC->GetCollisionObjectType() : -1, *Hit.BoneName.ToString(), Hit.Time, Hit.Distance, Hit.bStartPenetrating ? 1 : 0, Hit.PenetrationDepth,
		Hit.Normal.X, Hit.Normal.Y, Hit.Normal.Z, Hit.ImpactNormal.X, Hit.ImpactNormal.Y, Hit.ImpactNormal.Z, Hit.Location.X, Hit.Location.Y, Hit.Location.Z,
		Hit.ImpactPoint.X, Hit.ImpactPoint.Y, Hit.ImpactPoint.Z, Hit.TraceStart.X, Hit.TraceStart.Y, Hit.TraceStart.Z, Hit.TraceEnd.X, Hit.TraceEnd.Y, Hit.TraceEnd.Z,
		V.X, V.Y, V.Z, GL.X, GL.Y, GL.Z, GR.Pitch, GR.Yaw, GR.Roll);

	// L-2-c — 사람 메시에 맞으면 그 메시 물리 몸마다 한 줄: 세계 모양(WriteShapes 와 같은 칸) · 수류탄 쓸기를 막나 · 무기 채널을 막나.
	// 막나는 엔진 겹침 질의로 묻는다 — 몸마다 거름(물리 자산 CollisionReponse 와 메시 반응의 작은 쪽, BodyInstance.cpp BuildBodyFilterData)을 엔진이 그대로 쓴다.
	// 겹침 결과의 몸 번호는 메시 bMultiBodyOverlap 이 켜져야 채워진다(CollisionConversions.cpp — 꺼져 있으면 INDEX_NONE) → 질의 동안만 켜고 되돌린다.
	// 대조군 blockCtl = 같은 질의인데 질의 쪽이 메시 물체 종류를 무시(0 이어야 한다 — 질의가 거름을 타는지)
	// 수류탄 쪽 = 쓸린 성분(뿌리)의 물체 종류 · 반응(ProjectileMovement 의 SafeMoveUpdatedComponent 와 같은 거름). 무기 = Lyra_TraceChannel_Weapon(ECC_GameTraceChannel2) · 기본 반응
	USkeletalMeshComponent* Sk = Cast<USkeletalMeshComponent>(Hit.GetComponent());
	const UPrimitiveComponent* GC = SelfActor ? Cast<UPrimitiveComponent>(SelfActor->GetRootComponent()) : nullptr;
	if (Sk && GC)
	{
		const FCollisionResponseParams GrenResp(GC->GetCollisionResponseToChannels());
		FCollisionResponseParams CtlResp(GC->GetCollisionResponseToChannels());
		CtlResp.CollisionResponse.SetResponse(Sk->GetCollisionObjectType(), ECR_Ignore);
		FCollisionQueryParams QP(SCENE_QUERY_STAT(MOBMeshBody), false);
		const bool bMultiWas = Sk->bMultiBodyOverlap;
		Sk->bMultiBodyOverlap = true;
		const int32 CompGren = (int32)Sk->GetCollisionResponseToChannel(GC->GetCollisionObjectType());
		const int32 CompWeapon = (int32)Sk->GetCollisionResponseToChannel(ECC_GameTraceChannel2);
		const int32 GrenToMesh = (int32)GC->GetCollisionResponseToChannel(Sk->GetCollisionObjectType());
		for (int32 BI = 0; BI < Sk->Bodies.Num(); ++BI)
		{
			const FBodyInstance* Inst = Sk->Bodies[BI];
			const UBodySetup* BS = Inst ? Inst->GetBodySetup() : nullptr;
			if (!BS) continue;
			const FTransform BodyW = Inst->GetUnrealWorldTransform();
			int32 OvN = 0, OvSk = 0;	// 지나간 양 — 가장 최근 질의의 결과 수 · 그중 이 메시 몫
			const auto Blocks = [&](const FVector& At, ECollisionChannel Ch, const FCollisionResponseParams& Resp)
			{
				TArray<FOverlapResult> Out;
				GetWorld()->OverlapMultiByChannel(Out, At, FQuat::Identity, Ch, FCollisionShape::MakeSphere(0.1f), QP, Resp);
				int32 R = 0;	// 0 = 안 걸림 · 1 = 겹침만 · 2 = 막음
				OvN = Out.Num(); OvSk = 0;
				for (const FOverlapResult& O : Out)
				{
					if (O.GetComponent() != Sk) continue;
					++OvSk;
					if (O.GetItemIndex() == BI) R = FMath::Max(R, O.bBlockingHit ? 2 : 1);
				}
				return R;
			};
			const auto Emit = [&](const TCHAR* Kind, const FTransform& ElemLocal, double A, double B, double C)
			{
				const FTransform W = ElemLocal * BodyW;
				const FVector P = W.GetLocation(), X = W.GetUnitAxis(EAxis::X), Y = W.GetUnitAxis(EAxis::Y), Z = W.GetUnitAxis(EAxis::Z);
				const int32 BG = Blocks(P, GC->GetCollisionObjectType(), GrenResp);
				const int32 NG = OvN, SG = OvSk;
				const int32 BW = Blocks(P, ECC_GameTraceChannel2, FCollisionResponseParams::DefaultResponseParam);
				const int32 NW = OvN, SW = OvSk;
				const int32 BC = Blocks(P, GC->GetCollisionObjectType(), CtlResp);
				const int32 NC = OvN;
				UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_MESHBODY frame=%d name=%s hitBone=%s body=%d bone=%s setupResp=%d enabled=%d blockGren=%d blockWeapon=%d blockCtl=%d ")
					TEXT("ovGren=%d/%d ovWeapon=%d/%d ovCtl=%d compGren=%d compWeapon=%d grenToMesh=%d instGren=%d multiWas=%d kind=%s ")
					TEXT("p=%.6f,%.6f,%.6f x=%.7f,%.7f,%.7f y=%.7f,%.7f,%.7f z=%.7f,%.7f,%.7f a=%.6f b=%.6f c=%.6f scale=%.6f"),
					Frame, *GetNameSafe(SelfActor), *Hit.BoneName.ToString(), BI, *BS->BoneName.ToString(), (int32)BS->CollisionReponse, (int32)Inst->GetCollisionEnabled(), BG, BW, BC,
					SG, NG, SW, NW, NC, CompGren, CompWeapon, GrenToMesh, (int32)Inst->GetResponseToChannel(GC->GetCollisionObjectType()), bMultiWas ? 1 : 0, Kind,
					P.X, P.Y, P.Z, X.X, X.Y, X.Z, Y.X, Y.Y, Y.Z, Z.X, Z.Y, Z.Z, A, B, C, BodyW.GetScale3D().X);
			};
			for (const FKSphereElem& E : BS->AggGeom.SphereElems) Emit(TEXT("sphere"), FTransform(E.Center), E.Radius, 0, 0);
			for (const FKSphylElem& E : BS->AggGeom.SphylElems) Emit(TEXT("sphyl"), FTransform(E.Rotation, E.Center), E.Radius, E.Length, 0);
			for (const FKBoxElem& E : BS->AggGeom.BoxElems) Emit(TEXT("box"), FTransform(E.Rotation, E.Center), E.X, E.Y, E.Z);
		}
		Sk->bMultiBodyOverlap = bMultiWas;
	}
}

const AActor* UMOBContentProbeSubsystem::NearestGrenade(const FVector& Where, double& OutDist) const
{
	const AActor* Best = nullptr;
	OutDist = -1.0;
	for (const FGrenRec& R : Grens)
	{
		const AActor* A = R.A.Get();
		if (!A) continue;
		const double D = FVector::Dist(A->GetActorLocation(), Where);
		if (!Best || D < OutDist)
		{
			Best = A;
			OutDist = D;
		}
	}
	return Best;
}

// 튕김 — 엔진이 튕긴 속도를 셈한 바로 뒤(ProjectileMovementComponent HandleImpact). impactVel = 부딪힌 순간 속도, newVel = 튕긴 뒤(최대 속도 자르기 전) · 미끄러짐 깃발
void UMOBContentProbeSubsystem::OnGrenadeBounce(const FHitResult& ImpactResult, const FVector& ImpactVelocity)
{
	double D = -1.0;
	const AActor* G = NearestGrenade(ImpactResult.Location, D);
	const UProjectileMovementComponent* PM = G ? G->FindComponentByClass<UProjectileMovementComponent>() : nullptr;
	const FVector V = PM ? PM->Velocity : FVector::ZeroVector;
	FrameEvents += TEXT("g_bounce;");
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GBOUNCE frame=%d name=%s match=%.4f other=%s impactVel=%.6f,%.6f,%.6f newVel=%.6f,%.6f,%.6f normal=%.8f,%.8f,%.8f time=%.7f sliding=%d"),
		Frame, *GetNameSafe(G), D, *GetNameSafe(ImpactResult.GetActor()), ImpactVelocity.X, ImpactVelocity.Y, ImpactVelocity.Z, V.X, V.Y, V.Z,
		ImpactResult.Normal.X, ImpactResult.Normal.Y, ImpactResult.Normal.Z, ImpactResult.Time, (PM && PM->bIsSliding) ? 1 : 0);
}

// 멈춤 — 튕긴 뒤 속도가 멈춤 문턱(25) 아래라 걸음을 끝냄(엔진 StopSimulating)
void UMOBContentProbeSubsystem::OnGrenadeStop(const FHitResult& ImpactResult)
{
	double D = -1.0;
	const AActor* G = NearestGrenade(ImpactResult.Location, D);
	const FVector L = G ? G->GetActorLocation() : FVector::ZeroVector;
	FrameEvents += TEXT("g_stop;");
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GSTOP frame=%d name=%s match=%.4f other=%s loc=%.8f,%.8f,%.8f normal=%.8f,%.8f,%.8f"),
		Frame, *GetNameSafe(G), D, *GetNameSafe(ImpactResult.GetActor()), L.X, L.Y, L.Z, ImpactResult.Normal.X, ImpactResult.Normal.Y, ImpactResult.Normal.Z);
}

// 수류탄 판 매 프레임(내 틱 = 그 프레임 걸음 뒤) — 살아 있는 수류탄마다 GrenCsv 한 줄. 터짐 = 메시가 숨은 첫 프레임(Detonate 가 신호 → 숨김 → 날아감 끔 → 겹침 · 피해를
// 한 실행 줄에 다 한다), 사라짐 = 액터가 지워짐(꼬리 NS_Trail 이 끝난 뒤)
void UMOBContentProbeSubsystem::WatchGrenades()
{
	for (FGrenRec& R : Grens)
	{
		AActor* A = R.A.Get();
		if (!A)
		{
			if (!R.bGone)
			{
				R.bGone = true;
				FrameEvents += TEXT("g_gone;");
				UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GGONE frame=%d name=%s spawn=%d boom=%d"), Frame, *R.Name, R.SpawnFrame, R.BoomFrame);
			}
			continue;
		}
		const USceneComponent* Root = A->GetRootComponent();
		const bool bHidden = Root && Root->bHiddenInGame;
		const UProjectileMovementComponent* PM = A->FindComponentByClass<UProjectileMovementComponent>();
		if (bHidden && R.BoomFrame < 0)
		{
			R.BoomFrame = Frame;
			if (R.SpawnFrame == GrenSpawnFrame && GrenBoomFrame < 0) GrenBoomFrame = Frame;
			FrameEvents += TEXT("g_boom;");
			LogBoom(A, R.SpawnFrame);
		}
		const FVector L = A->GetActorLocation();
		const FRotator Rt = A->GetActorRotation();
		const FVector V = PM ? PM->Velocity : FVector::ZeroVector;
		GrenCsv += FString::Printf(TEXT("%d,%s,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%d,%d,%d,%d\n"), Frame, *R.Name, Frame - R.SpawnFrame, L.X, L.Y, L.Z, V.X, V.Y, V.Z,
			Rt.Pitch, Rt.Yaw, Rt.Roll, bHidden ? 1 : 0, (PM && PM->IsActive()) ? 1 : 0, (PM && PM->UpdatedComponent) ? 1 : 0, (PM && PM->bIsSliding) ? 1 : 0);
	}
}

// 터짐 다시 해 보기 — B_Grenade 「Detonate」 그대로(b_flow.txt): 수류탄 자리에서 반경 450 구 겹침(물체 종류 Pawn = ObjectTypeQuery3 · 액터 거르개 Pawn) →
// 사람마다 무시 목록 = 겹친 사람 전부에서 그 사람만 뺀 것 → 수류탄 → 그 사람 자리 선(TraceTypeQuery5 = Weapon_Capsule · 복잡 켬 · 수류탄 빼고) → 무엇이든 맞으면 피해 효과,
// 등급 = clamp(맞은 거리 ÷ 450, 0.1, 1). 피해 계산이 그 맞은 것으로 팀을 따져 벽이 먼저 맞으면 0 이다(G-0-b 실측 · LyraDamageExecution.cpp:67-109,144 — D107). 같은 구로 성분 겹침도 적는다(사람 몸에서 캡슐 · 메시 중 무엇이 겹치나 — 짐작 6). 체력은 앞 프레임 끝 값 > 지금 값
void UMOBContentProbeSubsystem::LogBoom(AActor* G, int32 SpawnFrame) const
{
	const FVector L = G->GetActorLocation();
	TArray<TEnumAsByte<EObjectTypeQuery>> Types;
	Types.Add(ObjectTypeQuery3);
	const TArray<AActor*> NoIgnore;
	TArray<AActor*> Over;
	UKismetSystemLibrary::SphereOverlapActors(G, L, GREN_RADIUS, Types, APawn::StaticClass(), NoIgnore, Over);
	TArray<UPrimitiveComponent*> Comps;
	UKismetSystemLibrary::SphereOverlapComponents(G, L, GREN_RADIUS, Types, nullptr, NoIgnore, Comps);
	FString CompS;
	for (const UPrimitiveComponent* C : Comps) CompS += FString::Printf(TEXT("%s/%s "), *GetNameSafe(C ? C->GetOwner() : nullptr), *GetNameSafe(C));
	const ACharacter* Ch = Me();
	const ACharacter* F = Foe.Get();
	// foeCmp = 나와 상대 팀 비교(ELyraTeamComparison — 0 같은 팀 · 1 다른 팀 · 2 못 셈). 같은 팀이면 피해 0 이 기대값(팀 규칙 · 묶음 10)
	const ULyraTeamSubsystem* TS = GetWorld()->GetSubsystem<ULyraTeamSubsystem>();
	int32 TeamMe = -1, TeamFoe = -1;
	const int32 FoeCmp = (TS && Ch && F) ? (int32)TS->CompareTeams(Ch, F, TeamMe, TeamFoe) : 2;
	// G-0-b2 — 무적 표식(나 · 상대) · 수류탄을 던진 몸(Instigator)과 그 몸에서 ASC 가 찾아지나(B_Grenade 가 피해 효과를 그 ASC 로 만든다 — 짐작 3) · 그 몸의 죽음 단계
	static const FGameplayTag Immune = FGameplayTag::RequestGameplayTag(TEXT("Gameplay.DamageImmunity"));
	const UAbilitySystemComponent* MA = FindASC(Ch);
	const UAbilitySystemComponent* FA = FindASC(F);
	const APawn* Inst = G->GetInstigator();
	const ULyraHealthComponent* InstHC = ULyraHealthComponent::FindHealthComponent(Inst);
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GBOOM frame=%d step=%s name=%s spawn=%d age=%d loc=%.6f,%.6f,%.6f over=%d comps=[%s] myHp=%.3f>%.3f foeHp=%.3f>%.3f foeCmp=%d foeCrouched=%d ")
		TEXT("myImmune=%d foeImmune=%d inst=%s instIsMe=%d instAsc=%d instDeath=%d die=%d"),
		Frame, bReady ? StepAt(ScriptIdx(), Step).Name : TEXT("-"), *G->GetName(), SpawnFrame, Frame - SpawnFrame, L.X, L.Y, L.Z, Over.Num(), *CompS.TrimEnd(),
		PrevMyHp, HealthOf(Ch), PrevFoeHp, HealthOf(F), FoeCmp, (F && F->bIsCrouched) ? 1 : 0,
		(MA && MA->HasMatchingGameplayTag(Immune)) ? 1 : 0, (FA && FA->HasMatchingGameplayTag(Immune)) ? 1 : 0, *GetNameSafe(Inst), (Inst && Inst == Ch) ? 1 : 0,
		FindASC(Inst) ? 1 : 0, InstHC ? (int32)InstHC->GetDeathState() : -1, GrenDieFrame);
	for (AActor* P : Over)
	{
		TArray<AActor*> Ignore = Over;
		Ignore.Remove(P);
		FHitResult Hit;
		const bool bHit = UKismetSystemLibrary::LineTraceSingle(G, L, P->GetActorLocation(), TraceTypeQuery5, true, Ignore, EDrawDebugTrace::None, Hit, true);
		// 캡슐 겉면까지 — 수류탄 중심에서 캡슐 축 선분까지 최단 거리 − 반지름(450 경계 · 벽 뒤 칸이 이것으로 가른다)
		double CapDist = -1.0;
		if (const ACharacter* PCh = Cast<ACharacter>(P))
		{
			const UCapsuleComponent* Cap = PCh->GetCapsuleComponent();
			const double R = Cap->GetScaledCapsuleRadius(), HH = Cap->GetScaledCapsuleHalfHeight();
			const FVector C = Cap->GetComponentLocation();
			CapDist = FMath::PointDistToSegment(L, C - FVector(0.0, 0.0, HH - R), C + FVector(0.0, 0.0, HH - R)) - R;
		}
		const FVector To = P->GetActorLocation();
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GTRACE frame=%d name=%s who=%s pawn=%s hit=%d dist=%.6f level=%.6f hitActor=%s hitComp=%s pen=%d to=%.6f,%.6f,%.6f centerDist=%.6f capDist=%.6f"),
			Frame, *G->GetName(), P == Ch ? TEXT("me") : (P == F ? TEXT("foe") : TEXT("other")), *GetNameSafe(P), bHit ? 1 : 0, Hit.Distance,
			FMath::Clamp((double)Hit.Distance / GREN_RADIUS, 0.1, 1.0), *GetNameSafe(Hit.GetActor()), *GetNameSafe(Hit.GetComponent()), Hit.bStartPenetrating ? 1 : 0,
			To.X, To.Y, To.Z, FVector::Dist(L, To), CapDist);
	}
}

// 쿨다운 남은 초 — GE_Grenade_Cooldown 효과(정의 클래스 이름으로 고른다 — 쿨다운 표식 이름을 몰라도 된다)
float UMOBContentProbeSubsystem::GrenadeCooldownLeft() const
{
	// 선수 상태 ASC(산 몸의 ASC 와 같은 것 — Lyra 는 ASC 가 선수 상태에 있다). 몸이 없을 때도 읽는다(G-0-b2)
	const UAbilitySystemComponent* A = MyPSASC();
	if (!A) A = FindASC(Me());
	if (!A) return -1.f;
	FGameplayEffectQuery Q;
	Q.CustomMatchDelegate.BindLambda([](const FActiveGameplayEffect& E) { return E.Spec.Def && E.Spec.Def->GetClass()->GetName().Contains(TEXT("GE_Grenade_Cooldown")); });
	float Left = 0.f;
	for (const float L : A->GetActiveEffectsTimeRemaining(Q)) Left = FMath::Max(Left, L);
	return Left;
}

// 대시 판 벽 칸(DashWall)과 같은 고르기 — 축 방향 넷 중 서 있는 면이 가장 가까운 것
int32 UMOBContentProbeSubsystem::NearestWallDir() const
{
	int32 Best = -1;
	for (int32 i = 0; i < 8; i += 2)
	{
		if (ClearDist[i] < DASH_CLEAR_TRACE && FMath::Abs(ClearNormalZ[i]) < DASH_WALL_NORMAL_Z && (Best < 0 || ClearDist[i] < ClearDist[Best])) Best = i;
	}
	return Best;
}

// 집에서 겨눌 면 찾기(무기 채널 선 · 나 · 상대 빼고). 눈에서 그 점까지 선이 안 막혀야 후보, 눈에서 가장 가까운 것.
// 비스듬한 면: 집 둘레 ±GREN_SLOPE_GRID 를 GREN_SLOPE_STEP 간격 격자로 위에서 내려 쏴 첫 면의 법선 Z 가 GREN_SLOPE_NZ_MIN~MAX 인 점.
//   (처음 판은 눈에서 요 · 피치를 돌려 쐈는데 g0a1 에서 하나도 못 찾았다 — 이 맵 비탈은 눈높이 아래 멀리 있고 상한 0.9 에도 걸렸다)
// 구석: 집 둘레 축 벽(ClearAround) a 마다 그 벽 GREN_CORNER_FROM_WALL 앞에서 벽을 따라 양옆(b = a ± 90도)으로 선을 쏴 서 있는 면(법선이 쏜 쪽을 마주봄)에
//   맞으면 두 면이 만나는 점(바닥 위 GREN_CORNER_Z). 처음 판은 이웃한 축 벽 둘을 집에서 바로 찾았는데 g0a1 집은 둘레 축 벽이 하나뿐(+X)이라 0 이었다.
//   던질 자리(OutStand) = 그 점에서 대각 바깥 GREN_CORNER_STAND 바닥 위 — 캡슐이 안 겹치고 그 자리 눈에서 보여야 후보. 집에서 던지면(g0a3, 2155cm) 거의 벽 하나로만 날아가
//   벽 하나 · 바닥만 맞았다. 대각으로 들어가야 두 벽을 잇달아 맞는다
bool UMOBContentProbeSubsystem::FindSurface(bool bCorner, FVector& OutPoint, FVector& OutStand, FString& OutWhat) const
{
	const ACharacter* Ch = Me();
	if (!Ch) return false;
	const UCapsuleComponent* Cap = Ch->GetCapsuleComponent();
	const FVector Body = Ch->GetActorLocation();
	const FVector Eye = Body + FVector(0.0, 0.0, Ch->BaseEyeHeight);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(MOBCProbeSurf), false, Ch);
	if (Foe.IsValid()) Params.AddIgnoredActor(Foe.Get());
	bool bFound = false;
	double BestD = 0.0;
	OutStand = FVector::ZeroVector;
	OutWhat = bCorner ? TEXT("corner none") : TEXT("slope none");
	// From → 점 선이 그 점 앞 20 안에서만 맞으면 보인다
	auto Visible = [&](const FVector& From, const FVector& Pt)
	{
		FHitResult V;
		return !GetWorld()->LineTraceSingleByChannel(V, From, Pt, Lyra_TraceChannel_Weapon, Params) || FVector::Dist(V.ImpactPoint, Pt) < 20.0;
	};
	if (!bCorner)
	{
		const int32 N = FMath::RoundToInt32(GREN_SLOPE_GRID / GREN_SLOPE_STEP);
		int32 Cands = 0, Hidden = 0;
		for (int32 ix = -N; ix <= N; ++ix)
		{
			for (int32 iy = -N; iy <= N; ++iy)
			{
				const FVector Top(Body.X + ix * GREN_SLOPE_STEP, Body.Y + iy * GREN_SLOPE_STEP, Body.Z + GREN_SLOPE_DROP_FROM);
				FHitResult H;
				if (!GetWorld()->LineTraceSingleByChannel(H, Top, Top - FVector(0.0, 0.0, 2.0 * GREN_SLOPE_DROP_FROM), Lyra_TraceChannel_Weapon, Params)) continue;
				const double Nz = H.ImpactNormal.Z;
				if (Nz < GREN_SLOPE_NZ_MIN || Nz > GREN_SLOPE_NZ_MAX) continue;
				++Cands;
				const FVector Pt = H.ImpactPoint + H.ImpactNormal * 5.0;
				const double D = FVector::Dist(Eye, Pt);
				if (bFound && D >= BestD) continue;
				if (!Visible(Eye, Pt))
				{
					++Hidden;
					continue;
				}
				bFound = true;
				BestD = D;
				OutPoint = Pt;
				OutWhat = FString::Printf(TEXT("slope at=%.1f,%.1f,%.1f dist=%.1f nz=%.4f actor=%s comp=%s"), Pt.X, Pt.Y, Pt.Z, D, Nz, *GetNameSafe(H.GetActor()), *GetNameSafe(H.GetComponent()));
			}
		}
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GSLOPE grid=%d cands=%d hidden=%d found=%d %s"), (2 * N + 1) * (2 * N + 1), Cands, Hidden, bFound ? 1 : 0, *OutWhat);
		return bFound;
	}
	const double R = Cap->GetScaledCapsuleRadius();
	const double HH = Cap->GetScaledCapsuleHalfHeight();
	const double FloorZ = Body.Z - HH;
	const FCollisionResponseParams Response(Cap->GetCollisionResponseToChannels());
	for (int32 a = 0; a < 8; a += 2)
	{
		if (ClearDist[a] >= DASH_CLEAR_TRACE || FMath::Abs(ClearNormalZ[a]) >= DASH_WALL_NORMAL_Z) continue;
		const FVector DA = KeyInput(a * 45.f);
		const double WallA = ClearDist[a] + R;	// 집 → 벽 a 면(축 방향 캡슐 쓸기 거리 + 반지름)
		const FVector S = Home + DA * (WallA - GREN_CORNER_FROM_WALL);
		for (const int32 b : { (a + 2) % 8, (a + 6) % 8 })
		{
			const FVector DB = KeyInput(b * 45.f);
			FHitResult H;
			const bool bHit = GetWorld()->LineTraceSingleByChannel(H, S, S + DB * GREN_SURF_TRACE, Lyra_TraceChannel_Weapon, Params);
			const bool bWallB = bHit && FMath::Abs(H.ImpactNormal.Z) < DASH_WALL_NORMAL_Z && (H.ImpactNormal | DB) < -0.7;
			FVector C = S + DA * GREN_CORNER_FROM_WALL + DB * H.Distance;
			C.Z = FloorZ + GREN_CORNER_Z;
			const FVector Diag = (DA + DB).GetSafeNormal();
			const FVector Aim = C - Diag * GREN_CORNER_INSET;
			// 던질 자리 — 대각 바깥 바닥(위에서 내려 쏨, 집 고르기와 같은 높이 규칙) · 캡슐이 아무것도 안 겹침 · 그 자리 눈에서 겨눔 점이 보임
			const FVector XY = Aim - Diag * GREN_CORNER_STAND;
			FHitResult Floor;
			bool bStand = bWallB && GetWorld()->LineTraceSingleByChannel(Floor, FVector(XY.X, XY.Y, Body.Z + 200.0), FVector(XY.X, XY.Y, Body.Z - 600.0), Cap->GetCollisionObjectType(), Params, Response);
			const FVector Stand(XY.X, XY.Y, Floor.ImpactPoint.Z + HH + 3.0);
			bStand = bStand && !GetWorld()->OverlapBlockingTestByChannel(Stand, FQuat::Identity, Cap->GetCollisionObjectType(), FCollisionShape::MakeCapsule(R, HH), Params, Response);
			const bool bSeen = bStand && Visible(Stand + FVector(0.0, 0.0, Ch->BaseEyeHeight), Aim);
			const double D = FVector::Dist(Eye, Aim);
			UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GCORNER a=%d b=%d hit=%d sideDist=%.1f normal=%.3f,%.3f,%.3f wallB=%d corner=%s stand=%s standOk=%d seen=%d dist=%.1f actor=%s"), a, b, bHit ? 1 : 0,
				H.Distance, H.ImpactNormal.X, H.ImpactNormal.Y, H.ImpactNormal.Z, bWallB ? 1 : 0, *C.ToString(), *Stand.ToString(), bStand ? 1 : 0, bSeen ? 1 : 0, D, *GetNameSafe(H.GetActor()));
			if (!bSeen || (bFound && D >= BestD)) continue;
			bFound = true;
			BestD = D;
			OutPoint = Aim;
			OutStand = Stand;
			OutWhat = FString::Printf(TEXT("corner a=%d b=%d at=%.1f,%.1f,%.1f stand=%.1f,%.1f,%.1f wallA=%.1f side=%.1f actorB=%s"), a, b, Aim.X, Aim.Y, Aim.Z, Stand.X, Stand.Y, Stand.Z, WallA,
				H.Distance, *GetNameSafe(H.GetActor()));
		}
	}
	return bFound;
}

// L-1-a 정한 자리 — (X, Y) 를 위 3000 에서 아래로 쏴(사람 캡슐 채널 · 나 · 상대 빼고) 첫 바닥 위 반높이 + 3 에 선다(구석 칸 던질 자리와 같은 높이 규칙).
// 캡슐이 무엇과 겹치면 못 선다(그 칸은 집에서 던진다 — GPLACE 줄 clear=0 으로 보인다)
bool UMOBContentProbeSubsystem::StandAt(double X, double Y, FVector& OutStand, FString& OutWhat) const
{
	const ACharacter* Ch = Me();
	if (!Ch) return false;
	const UCapsuleComponent* Cap = Ch->GetCapsuleComponent();
	const double R = Cap->GetScaledCapsuleRadius();
	const double HH = Cap->GetScaledCapsuleHalfHeight();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(MOBCProbeStand), false, Ch);
	if (Foe.IsValid()) Params.AddIgnoredActor(Foe.Get());
	const FCollisionResponseParams Response(Cap->GetCollisionResponseToChannels());
	FHitResult Floor;
	const bool bFloor = GetWorld()->LineTraceSingleByChannel(Floor, FVector(X, Y, 3000.0), FVector(X, Y, -3000.0), Cap->GetCollisionObjectType(), Params, Response);
	OutStand = FVector(X, Y, (bFloor ? Floor.ImpactPoint.Z : 0.0) + HH + 3.0);
	const bool bClear = bFloor && !GetWorld()->OverlapBlockingTestByChannel(OutStand, FQuat::Identity, Cap->GetCollisionObjectType(), FCollisionShape::MakeCapsule(R, HH), Params, Response);
	OutWhat = FString::Printf(TEXT("fixed stand=%.1f,%.1f,%.1f floor=%d clear=%d floorActor=%s"), OutStand.X, OutStand.Y, OutStand.Z, bFloor ? 1 : 0, bClear ? 1 : 0,
		bFloor ? *GetNameSafe(Floor.GetActor()) : TEXT("-"));
	return bClear;
}

// 카메라에서 그 점을 보게 — 몸 요 · 조종 회전을 돌린다(FaceTo). 카메라는 조종 회전을 따라 몸 둘레를 돌므로 누를 때까지 매 프레임 다시 불러 모은다
void UMOBContentProbeSubsystem::AimAt(const FVector& Point) const
{
	const ACharacter* Ch = Me();
	const APlayerController* PC = Ch ? Cast<APlayerController>(Ch->GetController()) : nullptr;
	if (!PC || !PC->PlayerCameraManager) return;
	const FRotator R = (Point - PC->PlayerCameraManager->GetCameraLocation()).Rotation();
	FaceTo(R.Pitch, R.Yaw);
}

// 던짐 칸 시작 — 자리 · 방향(GREN_SHOTS) · 나 · 상대 체력 1000. 겨눔 · 벽 거리 맞춤 · 누르기는 DuringGrenade
void UMOBContentProbeSubsystem::BeginGrenade()
{
	const FStep& S = StepAt(ScriptIdx(), Step);
	const FGrenShot& G = GrenShotAt(S.Arg);
	ACharacter* Ch = Me();
	GrenPressFrame = GrenSpawnFrame = GrenFirstHitFrame = GrenBoomFrame = GrenJumpFrame = GrenWallDir = GrenDieFrame = GrenPlacedFrame = -1;
	GrenHits = GrenSpawns = GrenAdjusts = 0;
	GrenCdAtPress = -1.f;
	GrenFirstHit.Reset();
	GrenWhere.Reset();
	bGrenAim = false;
	if ((int32)S.Arg >= GREN_LIFE_FIRST) LogScores(*FString::Printf(TEXT("%s_before"), S.Name));
	if (Ch) Ch->ConsumeMovementInputVector();	// 순간이동으로 시작 — 앞 칸 이동 입력을 비운다(대시 칸과 같은 까닭)
	SetHealthOf(Ch, 1000.f, G.MyHp > 0.f ? G.MyHp : 1000.f);
	SetHealthOf(Foe.Get(), 1000.f, G.FoeHp > 0.f ? G.FoeHp : 1000.f);
	if (bGrenL1B) DevicesOff(GetWorld(), S.Name);	// 나중에 불러온 장치가 있으면 그것도(다리 판은 매 프레임 찾는다)
	GoHome();
	switch (G.Place)
	{
	case EGrenPlace::Open:
		FaceTo(G.Pitch, 0.0);
		break;
	case EGrenPlace::WallCam:
	case EGrenPlace::WallGap:
	case EGrenPlace::WallBack:
		// 가장 가까운 벽 쪽 — 캡슐 겉면이 벽에서 Arg(벽 카메라 칸은 GREN_WALL_START_GAP) 떨어진 자리, 벽을 본다(등 칸은 벽을 등진다)
		GrenWallDir = NearestWallDir();
		if (Ch && GrenWallDir >= 0)
		{
			const FVector Dir = KeyInput(GrenWallDir * 45.f);
			const double Gap = G.Place == EGrenPlace::WallCam ? GREN_WALL_START_GAP : G.Arg;
			Ch->SetActorLocation(Home + Dir * FMath::Max(0.0, ClearDist[GrenWallDir] - Gap), false, nullptr, ETeleportType::TeleportPhysics);
			FaceTo(G.Pitch, GrenWallDir * 45.0 + (G.Place == EGrenPlace::WallBack ? 180.0 : 0.0));
		}
		GrenWhere = FString::Printf(TEXT("wallDir=%d wallDist=%.1f"), GrenWallDir, GrenWallDir >= 0 ? ClearDist[GrenWallDir] : -1.0);
		break;
	case EGrenPlace::FoeAt:
		PlaceFoe(G.Arg, true);
		bGrenAim = G.Arg > GREN_AIM_MIN_FOE && Foe.IsValid();
		if (bGrenAim) GrenAimPoint = Foe->GetActorLocation();
		else FaceTo(G.Pitch, 0.0);
		break;
	case EGrenPlace::Slope:
	case EGrenPlace::Corner:
	{
		FVector Stand;
		bGrenAim = FindSurface(G.Place == EGrenPlace::Corner, GrenAimPoint, Stand, GrenWhere);
		if (bGrenAim && Ch && !Stand.IsZero()) Ch->SetActorLocation(Stand, false, nullptr, ETeleportType::TeleportPhysics);
		if (!bGrenAim) FaceTo(0.0, 0.0);
		break;
	}
	case EGrenPlace::Fixed:
	{
		// L-1-a — 정한 자리에 서서(못 서면 집에서) 정한 점을 겨누거나 정한 요 · 피치로
		const FGrenFixed& X = GrenFixedAt(G.Arg);
		FVector Stand;
		if (Ch && StandAt(X.StandX, X.StandY, Stand, GrenWhere)) Ch->SetActorLocation(Stand, false, nullptr, ETeleportType::TeleportPhysics);
		bGrenAim = X.bAimPoint;
		if (bGrenAim) GrenAimPoint = FVector(X.AimX, X.AimY, X.AimZ);
		else FaceTo(X.Pitch, X.Yaw);
		break;
	}
	case EGrenPlace::Placed:
		FaceTo(0.0, 0.0);	// 집에서 수평 — 옮김은 DuringGrenade(생긴 프레임)
		break;
	case EGrenPlace::BlastHit:
	case EGrenPlace::BlastCap:
	case EGrenPlace::BlastCenter:
	case EGrenPlace::BlastCrouch:
	case EGrenPlace::WallLedge:
	case EGrenPlace::WallRoof:
	case EGrenPlace::SelfNear:
	case EGrenPlace::WallAt:
		// 던지는 동안 상대는 등 뒤 400(FoeSetup 자리) — 세우기는 터지기 전 BlastPass(SelfNear 는 나를 세운다)
		if (ACharacter* F = Foe.Get()) F->UnCrouch();
		PlaceFoe(-400.0, true);
		FaceTo(G.Pitch, 0.0);
		GrenBlastS = GrenBlastZ = 0.0;
		GrenBlastDir = FVector::ZeroVector;
		break;
	}
	GrenMyHpAtBegin = HealthOf(Ch);
	GrenFoeHpAtBegin = HealthOf(Foe.Get());
	// 섞어 누름 칸(G-0-c) — 칸 값을 비우고 능력 알림을 켠다(다른 칸에선 알림이 와도 안 적는다)
	GrenStepFrame = Frame;
	ComboOtherPress = ComboOtherStart = ComboOtherEnd = ComboOtherCancelled = -1;
	ComboQPresses = 0;
	bComboOtherWas = bComboQReleased = false;
	ComboMagChanges.Reset();
	ComboOns.Reset();
	ComboFails.Reset();
	bComboWatch = G.Combo != EGrenCombo::None;
	if (bComboWatch)
	{
		WatchAbilities();
		ComboMagBegin = ComboMagPrev = ActiveMagazine();
	}
	// 수류탄끼리 · 밟기 칸(G-0-c2) — 칸 값을 비우고, 던지는 두 칸은 쿨다운 효과를 지우고, 내 몸 막힘 알림(MOB_CPROBE_HIT)을 건다(칸 끝에 뗀다)
	TouchStopFrame = TouchPress2 = TouchDropFrame = TouchT = TouchHome = TouchCdRemoved = TouchCdRemoved2 = TouchG2Spawn = TouchAliveAtWalk = -1;
	TouchPawnHits = TouchPawnGrenHits = TouchOnGren = 0;
	TouchG1.Reset();
	TouchG2.Reset();
	TouchG1Name.Reset();
	TouchG2Name.Reset();
	TouchG1StopLoc = FVector::ZeroVector;
	bTouchWatch = G.Touch != EGrenTouch::None;
	if (G.Touch == EGrenTouch::Walk) TouchWalkStart = FVector::ZeroVector;
	if (G.Touch == EGrenTouch::Drop || G.Touch == EGrenTouch::Walk) TouchCdRemoved = ClearGrenadeCooldown();
	if (bTouchWatch && Ch && !bBump)
	{
		Ch->OnActorHit.AddUniqueDynamic(this, &UMOBContentProbeSubsystem::OnPawnHit);
		TouchHitPawn = Ch;
	}
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GPLACE step=%s shot=%s place=%d pitch=%.1f arg=%.1f run=%d jump=%d me=%s aim=%d aimAt=%s myHp=%.1f foeHp=%.1f %s"),
		S.Name, G.Name, (int32)G.Place, G.Pitch, G.Arg, G.bRun ? 1 : 0, G.bJump ? 1 : 0, Ch ? *Ch->GetActorLocation().ToString() : TEXT("-"), bGrenAim ? 1 : 0,
		*GrenAimPoint.ToString(), GrenMyHpAtBegin, GrenFoeHpAtBegin, *GrenWhere);
}

// 던짐 칸 한 프레임 — 겨눔(누를 때까지 매 프레임) · 벽 카메라 거리 맞춤(두 번) · 달리기 · 점프 · Q 누름(한 프레임)과 뗌.
// 여기서 누른 Q 는 다음 프레임 입력 처리에서 켜진다(대시 판과 같은 차례)
void UMOBContentProbeSubsystem::DuringGrenade()
{
	const FStep& S = StepAt(ScriptIdx(), Step);
	const FGrenShot& G = GrenShotAt(S.Arg);
	ACharacter* Ch = Me();
	// 피해 칸 — 이 칸 수류탄의 나이가 GREN_BLAST_AT · + STEP · + 2 STEP 인 프레임에 세움 · 고침 · 잼. 내 몸이 없어도 한다(던진 뒤 죽는 칸, G-0-b2)
	if (IsBlastPlace(G.Place) && GrenSpawnFrame >= 0)
	{
		const int32 Age = Frame - GrenSpawnFrame;
		for (const FGrenRec& R : Grens)
		{
			if (R.SpawnFrame != GrenSpawnFrame || !R.A.IsValid()) continue;
			for (int32 Pass = 0; Pass < 3; ++Pass)
			{
				if (Age == GREN_BLAST_AT + Pass * GREN_BLAST_STEP) BlastPass(R.A.Get(), Pass);
			}
		}
	}
	// 던진 뒤 죽는 칸(G-0-b2 · 묶음 11) — 이 칸 수류탄이 생기고 DieAfter 프레임 뒤 스스로 죽는다(무기 판 「die」 칸과 같은 DamageSelfDestruct)
	if (G.DieAfter >= 0 && GrenSpawnFrame >= 0 && GrenDieFrame < 0 && Frame - GrenSpawnFrame >= G.DieAfter && Ch)
	{
		if (ULyraHealthComponent* HC = ULyraHealthComponent::FindHealthComponent(Ch)) HC->DamageSelfDestruct();
		GrenDieFrame = Frame;
		FrameEvents += TEXT("die;");
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GDIE step=%s frame=%d age=%d hp=%.1f"), S.Name, Frame, Frame - GrenSpawnFrame, HealthOf(Ch));
	}
	// 던진 뒤 상대에게 죽는 칸(G-0-b3 · 묶음 20) — 이 칸 수류탄이 생기고 FoeKillAfter 프레임 뒤 상대가 나를 죽인다. 그 처치로 상대 팀이 낮춘 목표 점수에 닿아 끝 단계가 되고 수류탄은 그 뒤에 터진다
	if (G.FoeKillAfter >= 0 && GrenSpawnFrame >= 0 && GrenDieFrame < 0 && Frame - GrenSpawnFrame >= G.FoeKillAfter && Ch)
	{
		FoeKillMe(S.Name);
		GrenDieFrame = Frame;
		FrameEvents += TEXT("foekill;");
	}
	if (!Ch) return;
	if (G.Combo != EGrenCombo::None)
	{
		DuringGrenCombo();	// 섞어 누름 칸은 누름 · 뗌을 따로 한다(세움은 Open — 겨눔 · 벽 맞춤 · 달리기 · 점프 없음)
		return;
	}
	if (G.Touch != EGrenTouch::None)
	{
		DuringGrenTouch();	// 수류탄끼리 · 밟기 칸도 누름 · 옮김 · 걷기를 따로 한다(세움은 Open)
		return;
	}
	// 옮겨 놓기 칸(L-1-a) — 이 칸 수류탄을 생긴 그 프레임(나이 0 줄을 적은 뒤)에 정한 자리 · 속도로. 줄 꼴은 GDROP 과 같다(대조 도구가 그 프레임 걸음 뒤를 덮는다)
	// L-1-b 폭발 칸(WallAt)도 같은 길로 떨군다(GREN_WALLAT Drop 줄)
	if ((G.Place == EGrenPlace::Placed || G.Place == EGrenPlace::WallAt) && GrenSpawnFrame >= 0 && GrenPlacedFrame < 0)
	{
		for (const FGrenRec& R : Grens)
		{
			AActor* A = R.A.Get();
			UProjectileMovementComponent* PM = A ? A->FindComponentByClass<UProjectileMovementComponent>() : nullptr;
			if (R.SpawnFrame != GrenSpawnFrame || !A || !PM) continue;
			const FGrenPlaced& P = GrenPlacedAt(G.Place == EGrenPlace::WallAt ? (float)GrenWallAtOf(G.Arg).Drop : G.Arg);
			const FVector Before = A->GetActorLocation();
			A->SetActorLocation(FVector(P.X, P.Y, P.Z), false, nullptr, ETeleportType::TeleportPhysics);
			PM->Velocity = FVector(P.VX, P.VY, P.VZ);
			GrenPlacedFrame = Frame;
			FrameEvents += TEXT("g_place;");
			const FVector After = A->GetActorLocation();
			UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GDROP frame=%d step=%s g1=- g1Loc=0.000000,0.000000,0.000000 g2=%s g2Spawn=%d before=%.6f,%.6f,%.6f after=%.6f,%.6f,%.6f vel=%.6f,%.6f,%.6f active=%d rot=%.6f,%.6f,%.6f"),
				Frame, S.Name, *R.Name, R.SpawnFrame, Before.X, Before.Y, Before.Z, After.X, After.Y, After.Z,
				PM->Velocity.X, PM->Velocity.Y, PM->Velocity.Z, PM->UpdatedComponent ? 1 : 0, A->GetActorRotation().Pitch, A->GetActorRotation().Yaw, A->GetActorRotation().Roll);
			break;
		}
	}
	const bool bBeforePress = GrenPressFrame < 0;
	if (bGrenAim && bBeforePress) AimAt(GrenAimPoint);
	// 벽 카메라 거리 — 카메라에서 시선 방향 무기 채널 선(나 빼고)이 벽에 닿는 거리를 재 몸을 벽 쪽으로 (잰 거리 − Arg) 옮긴다.
	// 캡슐 겉면이 벽에서 GREN_WALL_MIN_GAP 보다 가까워지면 거기서 멈춘다(clamped=1 — 그 칸은 정한 카메라 거리가 안 된다)
	if (G.Place == EGrenPlace::WallCam && GrenWallDir >= 0 && bBeforePress && GrenAdjusts < 2 && StepTime >= (GrenAdjusts == 0 ? GREN_ADJUST_AT0 : GREN_ADJUST_AT1))
	{
		const APlayerController* PC = Cast<APlayerController>(Ch->GetController());
		const APlayerCameraManager* Cam = PC ? PC->PlayerCameraManager : nullptr;
		FHitResult H;
		bool bHit = false;
		FVector CL = FVector::ZeroVector;
		if (Cam)
		{
			CL = Cam->GetCameraLocation();
			FCollisionQueryParams Params(SCENE_QUERY_STAT(MOBCProbeWallCam), false, Ch);
			bHit = GetWorld()->LineTraceSingleByChannel(H, CL, CL + Cam->GetCameraRotation().Vector() * GREN_TRACE_RANGE, Lyra_TraceChannel_Weapon, Params);
		}
		const FVector Dir = KeyInput(GrenWallDir * 45.f);
		const double Along = (Ch->GetActorLocation() - Home) | Dir;
		const double MaxAlong = ClearDist[GrenWallDir] - GREN_WALL_MIN_GAP;
		const double NewAlong = bHit ? FMath::Min(Along + (H.Distance - G.Arg), MaxAlong) : Along;
		if (bHit) Ch->SetActorLocation(Ch->GetActorLocation() + Dir * (NewAlong - Along), false, nullptr, ETeleportType::TeleportPhysics);
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GADJUST step=%s n=%d hit=%d camDist=%.3f want=%.1f along=%.3f>%.3f clamped=%d actor=%s cam=%s"), S.Name, GrenAdjusts, bHit ? 1 : 0,
			H.Distance, G.Arg, Along, NewAlong, (bHit && NewAlong >= MaxAlong) ? 1 : 0, *GetNameSafe(H.GetActor()), *CL.ToString());
		++GrenAdjusts;
	}
	// 달리며 칸 — 칸 처음부터 누른 뒤 GREN_RUN_AFTER 프레임까지 몸 앞으로
	if (G.bRun && (bBeforePress || Frame <= GrenPressFrame + GREN_RUN_AFTER)) Ch->AddMovementInput(Ch->GetActorForwardVector(), 1.f);
	bool bPress = false;
	if (G.bJump)
	{
		if (GrenJumpFrame < 0 && StepTime >= GREN_JUMP_AT)
		{
			PressTag(TEXT("InputTag.Jump"), true);
			GrenJumpFrame = Frame;
			FrameEvents += TEXT("jump_press;");
		}
		else if (GrenJumpFrame >= 0 && Frame == GrenJumpFrame + 1)
		{
			PressTag(TEXT("InputTag.Jump"), false);
			FrameEvents += TEXT("jump_release;");
		}
		bPress = bBeforePress && GrenJumpFrame >= 0 && Frame >= GrenJumpFrame + GREN_JUMP_THROW_AFTER;
	}
	else
	{
		bPress = bBeforePress && StepTime >= GREN_PRESS_AT;
	}
	if (bPress)
	{
		PressTag(GREN_TAG, true);
		GrenPressFrame = Frame;
		GrenCdAtPress = GrenadeCooldownLeft();
		FrameEvents += TEXT("g_press;");
	}
	else if (GrenPressFrame >= 0 && Frame == GrenPressFrame + 1)
	{
		PressTag(GREN_TAG, false);
		FrameEvents += TEXT("g_release;");
	}
}

// 피해 칸 한 번 — Pass 0 에 상대를 세우고(벽 칸은 수류탄도 옮김 · 앉은 칸은 앉힘), 1 에 폭발 선 거리를 재서 거리 칸만 그만큼 고치고, 2 는 재기만 한다.
// 잼 = B_Grenade 「Detonate」와 같은 선(LogBoom — TraceTypeQuery5 · 복잡 켬 · 수류탄 빼고) · 캡슐 겉면 최단 거리 · 몸 가운데 수평 거리.
// Pass 0 의 잼은 옮긴 그 프레임 값인데도 새 자리를 본다(g0b1 — 앉은 칸 빼고 11/11 이 Pass 2 와 같음, 앉기는 다음 프레임 이동이 한다). 판정은 Pass 2 와 터짐 줄(GTRACE)로 한다
void UMOBContentProbeSubsystem::BlastPass(AActor* G, int32 Pass)
{
	const FStep& S = StepAt(ScriptIdx(), Step);
	const FGrenShot& Shot = GrenShotAt(S.Arg);
	const bool bSelf = Shot.Place == EGrenPlace::SelfNear;	// G-0-b2 자기 피해 · 자기 처치 — 상대 대신 나를 세운다
	ACharacter* F = bSelf ? Me() : Foe.Get();
	if (!G || !F) return;
	const UCapsuleComponent* Cap = F->GetCapsuleComponent();
	const double R = Cap->GetScaledCapsuleRadius();
	const UProjectileMovementComponent* PM = G->FindComponentByClass<UProjectileMovementComponent>();
	const bool bStopped = !PM || !PM->UpdatedComponent;
	const bool bWall = Shot.Place == EGrenPlace::WallLedge || Shot.Place == EGrenPlace::WallRoof;
	const bool bWallAt = Shot.Place == EGrenPlace::WallAt;	// L-1-b — 수류탄 · 상대 자리를 표(GREN_WALLAT)에서
	FCollisionQueryParams Params(SCENE_QUERY_STAT(MOBCProbeBlast), false, Me());
	Params.AddIgnoredActor(F);
	Params.AddIgnoredActor(G);
	const FCollisionResponseParams Response(Cap->GetCollisionResponseToChannels());
	// 그 자리 바닥 높이(상대 캡슐 채널 · 위에서 내려 쏨) — 못 찾으면 수류탄 높이 − 3(멈춘 수류탄은 바닥 위 약 3, g0a4 GSTOP z 52.4~53.4)
	const auto FloorAt = [&](double X, double Y, double& OutZ)
	{
		FHitResult H;
		const bool bHit = GetWorld()->LineTraceSingleByChannel(H, FVector(X, Y, 1000.0), FVector(X, Y, -200.0), Cap->GetCollisionObjectType(), Params, Response);
		OutZ = bHit ? H.ImpactPoint.Z : 0.0;
		return bHit;
	};
	FString Set;
	if (Pass == 0)
	{
		const double HH = Cap->GetScaledCapsuleHalfHeight();
		FVector L = G->GetActorLocation();
		double FloorZ = 0.0;
		bool bFloor = false;
		if (bWallAt)
		{
			// 수류탄 = 표 자리(공중이어도 멈춘 채 그대로), 상대 = 표 x · y · 수류탄을 본다. 바닥은 수류탄 높이 위 150 ~ 아래 300(FloorAt 의 연습장 높이 1000 ~ −200 은 L_Expanse 에 안 맞음)
			const FGrenWallAt& W = GrenWallAtOf(Shot.Arg);
			G->SetActorLocation(FVector(W.GX, W.GY, W.GZ), false, nullptr, ETeleportType::TeleportPhysics);
			L = G->GetActorLocation();
			const FVector D(W.FX - L.X, W.FY - L.Y, 0.0);
			GrenBlastS = D.Size();
			GrenBlastDir = D.GetSafeNormal();
			FHitResult FH;
			bFloor = GetWorld()->LineTraceSingleByChannel(FH, FVector(W.FX, W.FY, L.Z + 150.0), FVector(W.FX, W.FY, L.Z - 300.0), Cap->GetCollisionObjectType(), Params, Response);
			FloorZ = bFloor ? FH.ImpactPoint.Z : 0.0;
			Set = FString::Printf(TEXT("moved=1 at=%d floorHit=%d floorActor=%s topZ=%.3f"), (int32)Shot.Arg, bFloor ? 1 : 0, *GetNameSafe(FH.GetActor()), FloorZ);
		}
		else if (bWall)
		{
			// 수류탄 = 면 앞 GREN_WALL_BLAST_GAP 바닥(높이는 멈춘 그대로 — 같은 바닥 z 50), 상대 = 면 너머 Arg 윗면 · 수류탄을 본다
			const bool bLedge = Shot.Place == EGrenPlace::WallLedge;
			const FVector Out = bLedge ? FVector(0.0, 1.0, 0.0) : FVector(-1.0, 0.0, 0.0);	// 면에서 수류탄 쪽
			const FVector FacePt = bLedge ? FVector(GREN_LEDGE_X, GREN_LEDGE_FACE_Y, L.Z) : FVector(GREN_ROOF_FACE_X, GREN_ROOF_Y, L.Z);
			const FVector Gl = FacePt + Out * GREN_WALL_BLAST_GAP;
			G->SetActorLocation(Gl, false, nullptr, ETeleportType::TeleportPhysics);
			L = G->GetActorLocation();
			GrenBlastDir = -Out;
			GrenBlastS = GREN_WALL_BLAST_GAP + Shot.Arg;
			FHitResult FH;
			const bool bFace = GetWorld()->LineTraceSingleByChannel(FH, L, L - Out * 200.0, Lyra_TraceChannel_Weapon_Capsule, Params);
			bFloor = FloorAt(L.X - Out.X * GrenBlastS, L.Y - Out.Y * GrenBlastS, FloorZ);
			Set = FString::Printf(TEXT("moved=1 faceHit=%d faceDist=%.3f faceActor=%s topZ=%.3f"), bFace ? 1 : 0, FH.Distance, *GetNameSafe(FH.GetActor()), FloorZ);
		}
		else
		{
			GrenBlastDir = FVector(1.0, 0.0, 0.0);	// 집 반대쪽 — 던진 길 너머 빈 바닥
			bFloor = FloorAt(L.X + GrenBlastS, L.Y, FloorZ);
		}
		if (!bFloor) FloorZ = L.Z - 3.0;
		GrenBlastZ = FloorZ + HH + GREN_FOE_FLOOR_GAP;
		if (Shot.Place == EGrenPlace::BlastCenter || Shot.Place == EGrenPlace::BlastCrouch || bSelf)
		{
			GrenBlastS = Shot.Arg;
		}
		else if (Shot.Place == EGrenPlace::BlastCap)
		{
			// 수류탄이 축 선분 아래 끝보다 낮으면 가장 가까운 점 = 아래 끝 — 수평 = √((Arg + R)² − 높이 차²)
			const double Dz = FMath::Max(0.0, (GrenBlastZ - (HH - R)) - L.Z);
			GrenBlastS = FMath::Sqrt(FMath::Max(0.0, FMath::Square(Shot.Arg + R) - Dz * Dz));
		}
		else if (Shot.Place == EGrenPlace::BlastHit)
		{
			// 선이 캡슐에 닿는 거리는 수평 거리에 따라 는다 — 이분법으로 Arg 가 되는 수평 거리
			double Lo = R + 0.01, Hi = 2000.0;
			for (int32 i = 0; i < 60; ++i)
			{
				const double M = 0.5 * (Lo + Hi);
				const FVector C(L.X + GrenBlastDir.X * M, L.Y + GrenBlastDir.Y * M, GrenBlastZ);
				(RayCapsuleDist(L, C, R, HH) < Shot.Arg ? Lo : Hi) = M;
			}
			GrenBlastS = Hi;
		}
		const FRotator Face = (-GrenBlastDir).Rotation();
		F->SetActorLocationAndRotation(FVector(L.X + GrenBlastDir.X * GrenBlastS, L.Y + GrenBlastDir.Y * GrenBlastS, GrenBlastZ), FRotator(0.0, Face.Yaw, 0.0), false, nullptr, ETeleportType::TeleportPhysics);
		if (UCharacterMovementComponent* M = F->GetCharacterMovement()) M->StopMovementImmediately();
		if (AController* C = F->GetController()) C->SetControlRotation(FRotator(0.0, Face.Yaw, 0.0));
		if (Shot.Place == EGrenPlace::BlastCrouch) F->Crouch();
		if (bWall || bWallAt)
		{
			UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GWALLSET frame=%d step=%s grenade=%.6f,%.6f,%.6f foe=%s %s"), Frame, S.Name, L.X, L.Y, L.Z, *F->GetActorLocation().ToString(), *Set);
		}
	}
	const FVector L = G->GetActorLocation();
	const FVector To = F->GetActorLocation();
	FHitResult Hit;
	const TArray<AActor*> NoIgnore;
	const bool bHit = UKismetSystemLibrary::LineTraceSingle(G, L, To, TraceTypeQuery5, true, NoIgnore, EDrawDebugTrace::None, Hit, true);
	const double HHNow = Cap->GetScaledCapsuleHalfHeight();
	const FVector Cc = Cap->GetComponentLocation();
	const double CapDist = FMath::PointDistToSegment(L, Cc - FVector(0.0, 0.0, HHNow - R), Cc + FVector(0.0, 0.0, HHNow - R)) - R;
	const double Horiz = FVector::Dist2D(L, To);
	double Fix = 0.0;
	if (Pass == 1 && Shot.Place == EGrenPlace::BlastHit && bHit) Fix = Shot.Arg - Hit.Distance;
	if (Pass == 1 && Shot.Place == EGrenPlace::BlastCap) Fix = Shot.Arg - CapDist;
	if (Fix != 0.0)
	{
		GrenBlastS += Fix;
		F->SetActorLocation(FVector(L.X + GrenBlastDir.X * GrenBlastS, L.Y + GrenBlastDir.Y * GrenBlastS, To.Z), false, nullptr, ETeleportType::TeleportPhysics);
	}
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GBLAST frame=%d step=%s pass=%d place=%d target=%.3f stopped=%d grenade=%.6f,%.6f,%.6f foe=%.6f,%.6f,%.6f crouched=%d hh=%.3f ")
		TEXT("hit=%d dist=%.6f hitActor=%s hitComp=%s pen=%d capDist=%.6f centerDist=%.6f horiz=%.6f s=%.6f fix=%.6f who=%s"),
		Frame, S.Name, Pass, (int32)Shot.Place, Shot.Arg, bStopped ? 1 : 0, L.X, L.Y, L.Z, To.X, To.Y, To.Z, F->bIsCrouched ? 1 : 0, HHNow,
		bHit ? 1 : 0, Hit.Distance, *GetNameSafe(Hit.GetActor()), *GetNameSafe(Hit.GetComponent()), Hit.bStartPenetrating ? 1 : 0, CapDist, FVector::Dist(L, To), Horiz, GrenBlastS, Fix, bSelf ? TEXT("me") : TEXT("foe"));
}

// 던짐 칸 요약 한 줄(MOB_CPROBE_GREN) — 프레임 번호는 기록 표 frame 칸. spawnAfterPress = 생긴 프레임 − 누른 프레임, timerFrames = 터진 프레임 − 생긴 프레임(표 기대 120 또는 121)
void UMOBContentProbeSubsystem::EndGrenade()
{
	const FStep& S = StepAt(ScriptIdx(), Step);
	const FGrenShot& G = GrenShotAt(S.Arg);
	const ACharacter* Ch = Me();
	bool bGone = false;
	for (const FGrenRec& R : Grens)
	{
		if (GrenSpawnFrame >= 0 && R.SpawnFrame == GrenSpawnFrame) bGone = R.bGone;
	}
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GREN step=%s shot=%s place=%d press=%d spawn=%d spawnAfterPress=%d spawns=%d firstHit=%d hits=%d hitWhat=[%s] boom=%d timerFrames=%d gone=%d ")
		TEXT("cdAtPress=%.4f cdEnd=%.4f myDmg=%.3f foeDmg=%.3f crouched=%d where=[%s] die=%d"),
		S.Name, G.Name, (int32)G.Place, GrenPressFrame, GrenSpawnFrame, (GrenSpawnFrame >= 0 && GrenPressFrame >= 0) ? GrenSpawnFrame - GrenPressFrame : -1, GrenSpawns,
		GrenFirstHitFrame, GrenHits, *GrenFirstHit, GrenBoomFrame, (GrenBoomFrame >= 0 && GrenSpawnFrame >= 0) ? GrenBoomFrame - GrenSpawnFrame : -1, bGone ? 1 : 0,
		GrenCdAtPress, GrenadeCooldownLeft(), GrenMyHpAtBegin - HealthOf(Ch), GrenFoeHpAtBegin - HealthOf(Foe.Get()), (Ch && Ch->bIsCrouched) ? 1 : 0, *GrenWhere, GrenDieFrame);
	// 앉은 과녁 칸 — 다음 칸 전에 세운다
	if (G.Place == EGrenPlace::BlastCrouch && Foe.IsValid()) Foe->UnCrouch();
	if ((int32)S.Arg >= GREN_LIFE_FIRST) LogScores(S.Name);
	// 섞어 누름 칸(G-0-c) 요약 — 이 칸에서 생긴 수류탄 프레임 · 간격(짐작 8) · 다른 능력 프레임 · 탄창 바뀜 · 못 켬 까닭별 (수, 첫, 끝)
	if (G.Combo != EGrenCombo::None)
	{
		FString Spawns, Gaps;
		int32 Prev = -1;
		for (const FGrenRec& R : Grens)
		{
			if (R.SpawnFrame < GrenStepFrame) continue;
			Spawns += FString::Printf(TEXT("%d "), R.SpawnFrame);
			if (Prev >= 0) Gaps += FString::Printf(TEXT("%d "), R.SpawnFrame - Prev);
			Prev = R.SpawnFrame;
		}
		FString Fails;
		for (const TPair<FString, FIntVector>& KV : ComboFails) Fails += FString::Printf(TEXT("%s:%d:%d-%d "), *KV.Key, KV.Value.X, KV.Value.Y, KV.Value.Z);
		const FGrenComboSpec C = GrenComboSpec(G.Combo);
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GCOMBO step=%s combo=%s other=%s otherPress=%d otherStart=%d otherEnd=%d otherCancelled=%d qPresses=%d qFirst=%d spawns=[%s] gaps=[%s] ")
			TEXT("magBegin=%d magEnd=%d mag=[%s] ons=[%s] fails=[%s]"),
			S.Name, GrenComboName(G.Combo), C.OtherAbility ? C.OtherAbility : TEXT("-"), ComboOtherPress, ComboOtherStart, ComboOtherEnd, ComboOtherCancelled, ComboQPresses, GrenPressFrame,
			*Spawns.TrimEnd(), *Gaps.TrimEnd(), ComboMagBegin, ActiveMagazine(), *ComboMagChanges.TrimEnd(), *ComboOns.TrimEnd(), *Fails.TrimEnd());
		bComboWatch = false;
	}
	// 수류탄끼리 · 밟기 칸(G-0-c2) 요약 — 첫째 멈춤 · 지운 쿨다운 수 · 둘째 누름 · 생김 · 옮김 · 걷기 시작 · 집 · 내 몸 막힘 수(그중 수류탄) · 걸음 줄 중 수류탄 위 수 · 걷기 시작 때 산 수류탄 수
	if (G.Touch != EGrenTouch::None)
	{
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GTOUCH step=%s kind=%s g1=%s g1Stop=%d g1StopLoc=%.6f,%.6f,%.6f cdRemoved=%d cdRemoved2=%d press2=%d g2=%s g2Spawn=%d drop=%d ")
			TEXT("walkT=%d home=%d start=%.6f,%.6f,%.6f pawnHits=%d pawnGrenHits=%d onGren=%d aliveAtWalk=%d"),
			S.Name, GrenTouchName(G.Touch), TouchG1Name.IsEmpty() ? TEXT("-") : *TouchG1Name, TouchStopFrame, TouchG1StopLoc.X, TouchG1StopLoc.Y, TouchG1StopLoc.Z, TouchCdRemoved, TouchCdRemoved2,
			TouchPress2, TouchG2Name.IsEmpty() ? TEXT("-") : *TouchG2Name, TouchG2Spawn, TouchDropFrame, TouchT, TouchHome, TouchWalkStart.X, TouchWalkStart.Y, TouchWalkStart.Z,
			TouchPawnHits, TouchPawnGrenHits, TouchOnGren, TouchAliveAtWalk);
		if (ACharacter* P = TouchHitPawn.Get()) P->OnActorHit.RemoveDynamic(this, &UMOBContentProbeSubsystem::OnPawnHit);
		TouchHitPawn.Reset();
		bTouchWatch = false;
	}
}

// 수류탄끼리 · 밟기 칸 한 프레임(G-0-c2) — 첫 던짐(대조 칸 빼고) · 첫째 멈춤 보기 · Drop 은 둘째 누름과 옮김 ·
// Walk / WalkBare 는 순간이동 → 쉼 → 걷기 → 집, 그동안 매 프레임 MOB_CPROBE_GWALK(인덱스 = 순간이동 프레임에서 몇째)
void UMOBContentProbeSubsystem::DuringGrenTouch()
{
	const FStep& S = StepAt(ScriptIdx(), Step);
	const FGrenShot& G = GrenShotAt(S.Arg);
	ACharacter* Ch = Me();
	if (!Ch) return;
	if (G.Touch != EGrenTouch::WalkBare)
	{
		if (GrenPressFrame < 0 && StepTime >= GREN_PRESS_AT)
		{
			PressTag(GREN_TAG, true);
			GrenPressFrame = Frame;
			GrenCdAtPress = GrenadeCooldownLeft();
			FrameEvents += TEXT("g_press;");
		}
		else if (GrenPressFrame >= 0 && Frame == GrenPressFrame + 1)
		{
			PressTag(GREN_TAG, false);
			FrameEvents += TEXT("g_release;");
		}
		// 첫째가 멈춘 프레임 — 날아감 성분이 걸음을 끝내 UpdatedComponent 를 놓은 첫 틱(엔진 멈춤 알림 GSTOP 과 같은 프레임이어야 한다)
		if (TouchStopFrame < 0 && GrenSpawnFrame >= 0)
		{
			for (const FGrenRec& R : Grens)
			{
				AActor* A = R.A.Get();
				const UProjectileMovementComponent* PM = A ? A->FindComponentByClass<UProjectileMovementComponent>() : nullptr;
				if (R.SpawnFrame != GrenSpawnFrame || !PM || PM->UpdatedComponent) continue;
				TouchStopFrame = Frame;
				TouchG1 = A;
				TouchG1Name = R.Name;
				TouchG1StopLoc = A->GetActorLocation();
				FrameEvents += TEXT("t_stop;");
				LogGrenColl(A, TEXT("g1"));
				break;
			}
		}
	}
	if (G.Touch == EGrenTouch::Drop)
	{
		if (TouchStopFrame >= 0 && TouchPress2 < 0 && Frame >= TouchStopFrame + TOUCH_DROP_AFTER)
		{
			TouchCdRemoved2 = ClearGrenadeCooldown();
			PressTag(GREN_TAG, true);
			TouchPress2 = Frame;
			FrameEvents += TEXT("g_press2;");
		}
		else if (TouchPress2 >= 0 && Frame == TouchPress2 + 1)
		{
			PressTag(GREN_TAG, false);
			FrameEvents += TEXT("g_release2;");
		}
		// 둘째 — 둘째 누름 뒤에 생긴 수류탄을 생긴 그 프레임(내 틱)에 첫째 가운데 위 TOUCH_DROP_HEIGHT 로 옮기고 속도를 바로 아래로. 다음 프레임 걸음부터 떨어진다
		if (TouchPress2 >= 0 && TouchDropFrame < 0)
		{
			for (const FGrenRec& R : Grens)
			{
				AActor* A = R.A.Get();
				UProjectileMovementComponent* PM = A ? A->FindComponentByClass<UProjectileMovementComponent>() : nullptr;
				if (R.SpawnFrame <= TouchPress2 || !A || A == TouchG1.Get() || !PM) continue;
				const FVector Before = A->GetActorLocation();
				const FVector G1 = TouchG1.IsValid() ? TouchG1->GetActorLocation() : TouchG1StopLoc;
				A->SetActorLocation(G1 + FVector(0.0, 0.0, TOUCH_DROP_HEIGHT), false, nullptr, ETeleportType::TeleportPhysics);
				PM->Velocity = FVector(0.0, 0.0, -TOUCH_DROP_SPEED);
				TouchG2 = A;
				TouchG2Name = R.Name;
				TouchG2Spawn = R.SpawnFrame;
				TouchDropFrame = Frame;
				FrameEvents += TEXT("t_drop;");
				const FVector After = A->GetActorLocation();
				UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GDROP frame=%d step=%s g1=%s g1Loc=%.6f,%.6f,%.6f g2=%s g2Spawn=%d before=%.6f,%.6f,%.6f after=%.6f,%.6f,%.6f vel=%.6f,%.6f,%.6f active=%d"),
					Frame, S.Name, *TouchG1Name, G1.X, G1.Y, G1.Z, *R.Name, R.SpawnFrame, Before.X, Before.Y, Before.Z, After.X, After.Y, After.Z,
					PM->Velocity.X, PM->Velocity.Y, PM->Velocity.Z, PM->UpdatedComponent ? 1 : 0);
				LogGrenColl(A, TEXT("g2"));
				break;
			}
		}
		return;
	}
	// Walk — 첫째가 멈춘 다음 프레임에 걷기 시작 자리를 정한다(첫째 뒤 −X 로 TOUCH_WALK_BACK · 내 캡슐 채널로 위에서 내려 쏜 바닥 + 반높이 + 이동이 띄워 두는 틈)
	if (G.Touch == EGrenTouch::Walk && TouchStopFrame >= 0 && TouchT < 0 && Frame == TouchStopFrame + 1)
	{
		const UCapsuleComponent* Cap = Ch->GetCapsuleComponent();
		const FVector P(TouchG1StopLoc.X - TOUCH_WALK_BACK, TouchG1StopLoc.Y, TouchG1StopLoc.Z);
		FCollisionQueryParams Params(SCENE_QUERY_STAT(MOBCProbeTouchFloor), false, Ch);
		FHitResult H;
		const bool bFloor = GetWorld()->LineTraceSingleByChannel(H, P + FVector(0.0, 0.0, 200.0), P - FVector(0.0, 0.0, 200.0), Cap->GetCollisionObjectType(), Params,
			FCollisionResponseParams(Cap->GetCollisionResponseToChannels()));
		TouchWalkStart = FVector(P.X, P.Y, (bFloor ? H.ImpactPoint.Z : P.Z - 3.0) + Cap->GetScaledCapsuleHalfHeight() + GREN_FOE_FLOOR_GAP);
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GWALKSET frame=%d step=%s floor=%d floorZ=%.6f floorActor=%s start=%.6f,%.6f,%.6f"),
			Frame, S.Name, bFloor ? 1 : 0, H.ImpactPoint.Z, *GetNameSafe(H.GetActor()), TouchWalkStart.X, TouchWalkStart.Y, TouchWalkStart.Z);
		StartTouchWalk();
	}
	else if (G.Touch == EGrenTouch::WalkBare && TouchT < 0 && StepTime >= GREN_PRESS_AT && !TouchWalkStart.IsZero())
	{
		StartTouchWalk();
	}
	if (TouchT < 0 || TouchHome >= 0) return;
	// 이 프레임 걸음 뒤 상태(앞 프레임에 넣은 입력으로 움직인 결과). 두 칸이 같은 인덱스에 같은 입력을 넣으므로 인덱스끼리 견준다
	const int32 Idx = Frame - TouchT;
	const UCharacterMovementComponent* M = Ch->GetCharacterMovement();
	const FVector L = Ch->GetActorLocation();
	const FVector V = M ? M->Velocity : FVector::ZeroVector;
	const FHitResult* FH = M ? &M->CurrentFloor.HitResult : nullptr;
	const bool bOnGren = FH && IsGrenadeActor(FH->GetActor());
	if (bOnGren) ++TouchOnGren;
	const bool bInput = Idx >= TOUCH_WALK_SETTLE && Idx < TOUCH_WALK_SETTLE + TOUCH_WALK_FRAMES;
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GWALK step=%s idx=%d frame=%d me=%.6f,%.6f,%.6f vel=%.6f,%.6f,%.6f mode=%d floor=%d walkable=%d floorDist=%.6f floorActor=%s floorComp=%s onGren=%d input=%d"),
		S.Name, Idx, Frame, L.X, L.Y, L.Z, V.X, V.Y, V.Z, M ? (int32)M->MovementMode.GetValue() : -1, (M && M->CurrentFloor.bBlockingHit) ? 1 : 0,
		(M && M->CurrentFloor.bWalkableFloor) ? 1 : 0, M ? M->CurrentFloor.FloorDist : -1.f, FH ? *GetNameSafe(FH->GetActor()) : TEXT("-"), FH ? *GetNameSafe(FH->GetComponent()) : TEXT("-"),
		bOnGren ? 1 : 0, bInput ? 1 : 0);
	if (Idx >= TOUCH_WALK_SETTLE + TOUCH_WALK_FRAMES)
	{
		GoHome();
		TouchHome = Frame;
		FrameEvents += TEXT("t_home;");
	}
	else if (bInput)
	{
		Ch->AddMovementInput(FVector(1.0, 0.0, 0.0), 1.f);
	}
}

// 걷기 시작 — 앞 칸 이동 입력을 비우고 시작 자리로 순간이동 · 멈춤 · 요 0(+X 를 본다). 두 칸이 같은 일을 한다
void UMOBContentProbeSubsystem::StartTouchWalk()
{
	ACharacter* Ch = Me();
	if (!Ch) return;
	Ch->ConsumeMovementInputVector();
	Ch->SetActorLocation(TouchWalkStart, false, nullptr, ETeleportType::TeleportPhysics);
	if (UCharacterMovementComponent* M = Ch->GetCharacterMovement()) M->StopMovementImmediately();
	FaceTo(0.0, 0.0);
	TouchT = Frame;
	TouchAliveAtWalk = 0;
	for (const FGrenRec& R : Grens)
	{
		if (R.A.IsValid()) ++TouchAliveAtWalk;
	}
	FrameEvents += TEXT("t_walk;");
}

// 수류탄 성분마다 충돌 설정 — 켬 · 물체 종류 · 프로필 · 채널 0~31 반응(0 무시 · 1 겹침 · 2 막음) · 구 반지름 · 물리 · 사람 올라섬 허용(ECanBeCharacterBase) · 움직일 때 뺄 액터 수,
// 그리고 내 캡슐이 그 종류를 어떻게 받나 · 그 성분이 내 캡슐 종류를 어떻게 받나(둘 다 막음이어야 걸음이 막힌다)
void UMOBContentProbeSubsystem::LogGrenColl(const AActor* G, const TCHAR* Which) const
{
	if (!G) return;
	const ACharacter* Ch = Me();
	const UCapsuleComponent* Cap = Ch ? Ch->GetCapsuleComponent() : nullptr;
	TInlineComponentArray<UPrimitiveComponent*> Comps(G);
	for (const UPrimitiveComponent* P : Comps)
	{
		FString Resp;
		for (int32 c = 0; c < 32; ++c) Resp.AppendInt((int32)P->GetCollisionResponseToChannel((ECollisionChannel)c));
		const USphereComponent* Sp = Cast<USphereComponent>(P);
		const ECollisionChannel OT = P->GetCollisionObjectType();
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GCOLL frame=%d step=%s which=%s name=%s comp=%s class=%s root=%d enabled=%d objType=%d profile=%s resp=%s sphere=%.4f bounds=%.4f physics=%d stepUpOn=%d ignore=%d ")
			TEXT("capRespToIt=%d itRespToCap=%d capObjType=%d"),
			Frame, bReady ? StepAt(ScriptIdx(), Step).Name : TEXT("-"), Which, *G->GetName(), *P->GetName(), *P->GetClass()->GetName(), P == G->GetRootComponent() ? 1 : 0,
			(int32)P->GetCollisionEnabled(), (int32)OT, *P->GetCollisionProfileName().ToString(), *Resp, Sp ? Sp->GetScaledSphereRadius() : -1.f, P->Bounds.SphereRadius,
			P->IsSimulatingPhysics() ? 1 : 0, (int32)P->CanCharacterStepUpOn.GetValue(), P->GetMoveIgnoreActors().Num(),
			Cap ? (int32)Cap->GetCollisionResponseToChannel(OT) : -1, Cap ? (int32)P->GetCollisionResponseToChannel(Cap->GetCollisionObjectType()) : -1, Cap ? (int32)Cap->GetCollisionObjectType() : -1);
	}
}

// 쿨다운 효과 지우기 — GrenadeCooldownLeft 와 같은 고르기(정의 클래스 이름 GE_Grenade_Cooldown). 줄 = MOB_CPROBE_GCDCLEAR(지운 수 · 지운 뒤 남은 초)
int32 UMOBContentProbeSubsystem::ClearGrenadeCooldown()
{
	UAbilitySystemComponent* A = MyPSASC();
	if (!A) A = FindASC(Me());
	if (!A) return -1;
	FGameplayEffectQuery Q;
	Q.CustomMatchDelegate.BindLambda([](const FActiveGameplayEffect& E) { return E.Spec.Def && E.Spec.Def->GetClass()->GetName().Contains(TEXT("GE_Grenade_Cooldown")); });
	const int32 N = A->RemoveActiveEffects(Q);
	FrameEvents += TEXT("t_cdclear;");
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GCDCLEAR frame=%d step=%s removed=%d left=%.4f"), Frame, bReady ? StepAt(ScriptIdx(), Step).Name : TEXT("-"), N, GrenadeCooldownLeft());
	return N;
}

bool UMOBContentProbeSubsystem::IsGrenadeActor(const AActor* A)
{
	return A && A->GetClass()->GetName().StartsWith(TEXT("B_Grenade"));
}

// 섞어 누름 칸 한 프레임(G-0-c) — 다른 능력 켜짐 가장자리 · 탄창 바뀜을 보고, 칸 종류대로 Q 와 다른 능력을 누르고 뗀다(여기서 누른 것은 다음 프레임 입력 처리에서 켜진다)
void UMOBContentProbeSubsystem::DuringGrenCombo()
{
	const FStep& S = StepAt(ScriptIdx(), Step);
	const FGrenShot& G = GrenShotAt(S.Arg);
	const FGrenComboSpec C = GrenComboSpec(G.Combo);
	if (C.OtherAbility)
	{
		const bool bOn = IsAbilityActive(C.OtherAbility);
		if (bOn && !bComboOtherWas)
		{
			if (ComboOtherStart < 0) ComboOtherStart = Frame;
			FrameEvents += TEXT("other_start;");
		}
		else if (!bOn && bComboOtherWas)
		{
			if (ComboOtherEnd < 0) ComboOtherEnd = Frame;
			FrameEvents += TEXT("other_end;");
		}
		bComboOtherWas = bOn;
	}
	const int32 Mag = ActiveMagazine();
	if (Mag != ComboMagPrev)
	{
		ComboMagChanges += FString::Printf(TEXT("%d:%d>%d "), Frame, ComboMagPrev, Mag);
		ComboMagPrev = Mag;
	}
	const auto PressQ = [this]()
	{
		PressTag(GREN_TAG, true);
		if (GrenPressFrame < 0)
		{
			GrenPressFrame = Frame;
			GrenCdAtPress = GrenadeCooldownLeft();
		}
		++ComboQPresses;
		FrameEvents += TEXT("g_press;");
	};
	const auto PressOther = [this, &C](bool bPress)
	{
		PressTag(C.OtherTag, bPress);
		if (bPress) ComboOtherPress = Frame;
		FrameEvents += bPress ? TEXT("other_press;") : TEXT("other_release;");
	};
	switch (G.Combo)
	{
	case EGrenCombo::Spam:
		// 매 프레임 누름(떼지 않음 — 대시 판 DashSpam 과 같음). 그친 프레임에 한 번 뗀다
		if (StepTime >= GREN_PRESS_AT && StepTime < GREN_SPAM_UNTIL) PressQ();
		else if (GrenPressFrame >= 0 && !bComboQReleased)
		{
			PressTag(GREN_TAG, false);
			bComboQReleased = true;
			FrameEvents += TEXT("g_release;");
		}
		break;
	case EGrenCombo::QFire:
	case EGrenCombo::FireQ:
	case EGrenCombo::MeleeQSame:	// L-1-c 같은 프레임 넷도 같은 길 — 이름 차례대로 누르고 다음 프레임에 둘 다 뗀다
	case EGrenCombo::QMeleeSame:
	case EGrenCombo::ReloadQSame:
	case EGrenCombo::QReloadSame:
		if (GrenPressFrame < 0 && StepTime >= GREN_PRESS_AT)
		{
			if (G.Combo == EGrenCombo::QFire || G.Combo == EGrenCombo::QMeleeSame || G.Combo == EGrenCombo::QReloadSame) { PressQ(); PressOther(true); }
			else { PressOther(true); PressQ(); }
		}
		else if (GrenPressFrame >= 0 && Frame == GrenPressFrame + 1)
		{
			PressTag(GREN_TAG, false);
			FrameEvents += TEXT("g_release;");
			PressOther(false);
		}
		break;
	case EGrenCombo::ReloadQ:
	case EGrenCombo::DashQ:
	case EGrenCombo::MeleeQ:
		if (ComboOtherPress < 0 && StepTime >= GREN_PRESS_AT) PressOther(true);
		else if (ComboOtherPress >= 0 && Frame == ComboOtherPress + 1) PressOther(false);
		if (GrenPressFrame < 0 && ComboOtherStart >= 0 && Frame >= ComboOtherStart + C.QAfter) PressQ();
		else if (GrenPressFrame >= 0 && Frame == GrenPressFrame + 1)
		{
			PressTag(GREN_TAG, false);
			FrameEvents += TEXT("g_release;");
		}
		break;
	case EGrenCombo::None:
		break;
	}
}

// 내 선수 상태 ASC 에 능력 알림 셋 — 켜짐(AbilityActivatedCallbacks) · 꺼짐(OnAbilityEnded — 취소 깃발) · 못 켬(AbilityFailedCallbacks — 까닭 태그).
// ASC 는 선수 상태에 있어 부활해도 같다(g0c2 — 부활 넷 뒤 c_spam 에서 건 ASC 주인 = LyraPlayerState_0). 다르면 옛 것에서 떼고 새로 건다
void UMOBContentProbeSubsystem::WatchAbilities()
{
	UAbilitySystemComponent* A = MyPSASC();
	if (!A || A == ComboAsc.Get()) return;
	if (UAbilitySystemComponent* Old = ComboAsc.Get())
	{
		Old->AbilityActivatedCallbacks.Remove(AbilOnHandle);
		Old->OnAbilityEnded.Remove(AbilEndHandle);
		Old->AbilityFailedCallbacks.Remove(AbilFailHandle);
	}
	ComboAsc = A;
	AbilOnHandle = A->AbilityActivatedCallbacks.AddUObject(this, &UMOBContentProbeSubsystem::OnAbilOn);
	AbilEndHandle = A->OnAbilityEnded.AddUObject(this, &UMOBContentProbeSubsystem::OnAbilEnd);
	AbilFailHandle = A->AbilityFailedCallbacks.AddUObject(this, &UMOBContentProbeSubsystem::OnAbilFail);
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GABIL_WATCH frame=%d asc=%s owner=%s"), Frame, *GetNameSafe(A), *GetNameSafe(A->GetOwner()));
	// 능력마다 켜는 방식(0 = 누른 프레임 · 1 = 누르고 있는 동안 · 2 = 생길 때) — Lyra 입력 처리는 「누르고 있는 동안」 능력을 먼저 모으고 「누른 프레임」 능력을 뒤에 모아
	// 그 차례로 켠다(LyraAbilitySystemComponent.cpp:232-283). 같은 프레임 Q + 사격이 누른 차례와 상관없이 사격 먼저 켜지면(g0c1) 사격이 1 이어야 한다
	for (const FGameplayAbilitySpec& Spec : A->GetActivatableAbilities())
	{
		const ULyraGameplayAbility* L = Cast<ULyraGameplayAbility>(Spec.Ability);
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GABIL_SPEC ability=%s policy=%d tags=[%s]"), *GetNameSafe(Spec.Ability ? Spec.Ability->GetClass() : nullptr),
			L ? (int32)L->GetActivationPolicy() : -1, *Spec.GetDynamicSpecSourceTags().ToStringSimple());
	}
}

// 한 프레임 안 알림 차례(프레임이 바뀌면 0 부터)
int32 UMOBContentProbeSubsystem::NextAbilSeq()
{
	if (AbilSeqFrame != Frame)
	{
		AbilSeqFrame = Frame;
		AbilSeq = 0;
	}
	return AbilSeq++;
}

void UMOBContentProbeSubsystem::OnAbilOn(UGameplayAbility* Ability)
{
	if (!bComboWatch) return;
	const FString Name = GetNameSafe(Ability ? Ability->GetClass() : nullptr);
	ComboOns += FString::Printf(TEXT("%d:%s "), Frame, *Name);
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GABIL frame=%d seq=%d step=%s ev=on ability=%s"), Frame, NextAbilSeq(), StepAt(ScriptIdx(), Step).Name, *Name);
}

void UMOBContentProbeSubsystem::OnAbilEnd(const FAbilityEndedData& Data)
{
	if (!bComboWatch) return;
	const FString Name = GetNameSafe(Data.AbilityThatEnded ? Data.AbilityThatEnded->GetClass() : nullptr);
	const FGrenComboSpec C = bGrenade ? GrenComboSpec(GrenShotAt(StepAt(ScriptIdx(), Step).Arg).Combo) : FGrenComboSpec{ nullptr, nullptr, -1 };	// 대시 판(L-1-c)의 Arg 는 수류탄 칸 표가 아니다
	if (C.OtherAbility && ComboOtherCancelled < 0 && Name.Contains(C.OtherAbility)) ComboOtherCancelled = Data.bWasCancelled ? 1 : 0;
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GABIL frame=%d seq=%d step=%s ev=end ability=%s cancelled=%d"), Frame, NextAbilSeq(), StepAt(ScriptIdx(), Step).Name, *Name,
		Data.bWasCancelled ? 1 : 0);
}

// 못 켬은 매 프레임 Q 칸에서 쿨다운 동안 프레임마다 온다 — 까닭(능력 + 태그)마다 첫 번만 줄로 적고 나머지는 셈만(요약 GCOMBO fails)
void UMOBContentProbeSubsystem::OnAbilFail(const UGameplayAbility* Ability, const FGameplayTagContainer& Tags)
{
	if (!bComboWatch) return;
	const FString Key = FString::Printf(TEXT("%s[%s]"), *GetNameSafe(Ability ? Ability->GetClass() : nullptr), *Tags.ToStringSimple());
	FIntVector* V = ComboFails.Find(Key);
	if (V)
	{
		++V->X;
		V->Z = Frame;
		return;
	}
	ComboFails.Add(Key, FIntVector(1, Frame, Frame));
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_GABIL frame=%d seq=%d step=%s ev=fail what=%s cd=%.4f"), Frame, NextAbilSeq(), StepAt(ScriptIdx(), Step).Name, *Key, GrenadeCooldownLeft());
}
