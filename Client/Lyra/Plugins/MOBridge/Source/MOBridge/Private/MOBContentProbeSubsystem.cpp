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
#include "UnrealClient.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"

#include "AbilitySystem/Abilities/LyraAbilitySimpleFailureMessage.h"
#include "AbilitySystem/Attributes/LyraHealthSet.h"
#include "AbilitySystem/LyraAbilitySystemComponent.h"
#include "Camera/LyraCameraMode.h"
#include "Character/LyraCharacter.h"
#include "Character/LyraHealthComponent.h"
#include "Equipment/LyraQuickBarComponent.h"
#include "Inventory/LyraInventoryItemDefinition.h"
#include "Inventory/LyraInventoryItemInstance.h"
#include "Inventory/LyraInventoryManagerComponent.h"
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
	// 키보드 같은 이동 입력 — 각을 축마다 반올림(45도 = (1, 1), 길이 √2. Lyra 영웅 입력은 앞 · 옆을 따로 더한다)
	FVector KeyInput(float AngleDeg)
	{
		const double A = FMath::DegreesToRadians((double)AngleDeg);
		return FVector(FMath::RoundToDouble(FMath::Cos(A)), FMath::RoundToDouble(FMath::Sin(A)), 0.0);
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

	// 판 번호 0 = 연습 · 1 = 받침 · 2 = 다리 · 3 = 부딪힘(연습장) · 4 = 무기 받침 · 5 = 대시(연습장)
	const FStep& StepAt(int32 Script, int32 I)
	{
		switch (Script)
		{
		case 1: return PADS_SCRIPT[I];
		case 2: return BRIDGE_SCRIPT[I];
		case 3: return BUMP_SCRIPT[I];
		case 4: return WEAPONS_SCRIPT[I];
		case 5: return DASH_SCRIPT[I];
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
	bWeapons = Mode == TEXT("weapons");
	bDash = Mode == TEXT("dash");
	bForceQuinn = FParse::Param(Cmd, TEXT("MOBCPQuinn"));
	StartRealAt = FPlatformTime::Seconds();
	GAreScreenMessagesEnabled = false;
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_INIT world=%s tag=%s mode=%s"), *GetNameSafe(GetWorld()), *Tag,
		bBridge ? TEXT("bridge") : (bPads ? TEXT("pads") : (bBump ? TEXT("bump") : (bWeapons ? TEXT("weapons") : (bDash ? TEXT("dash") : TEXT("gym"))))));
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
	// 다리 판은 봇에게 죽어 다시 생기는 것도 지금 클라의 모습이라 적기만 하고 이어 간다. 무기 판은 죽음 뒤 가방을 보려고 스스로 죽으니 이어 간다
	if (Me() != ClearPawn.Get())
	{
		UE_LOG(LogMOBCProbe, Warning, TEXT("MOB_CPROBE_PAWN_CHANGED step=%d from=%s to=%s t=%.4f"), Step, *GetNameSafe(ClearPawn.Get()), *GetNameSafe(Me()), GetWorld()->GetTimeSeconds());
		if (!bBridge && !bWeapons)
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
		if (AAIController* AI = Foe.IsValid() ? Cast<AAIController>(Foe->GetController()) : nullptr)
		{
			if (AI->BrainComponent) AI->BrainComponent->StopLogic(TEXT("MOBContentProbe"));
			AI->StopMovement();
			AI->ClearFocus(EAIFocusPriority::Gameplay);
			AI->bSetControlRotationFromPawnOrientation = false;
		}
		GoHome();
		PlaceFoe(-400.0, true);
		// 대시 판 — 상대는 대시 길(뒤 대시가 등 뒤 400 을 지난다)을 막지 않게 숨기고, 집 둘레에서 여덟 방향이 빈 자리를 집으로 고른다(MOB_CPROBE_DASHHOME · CLEAR 줄)
		if (bDash)
		{
			SetFoeActive(false);
			PickDashHome();
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
	}
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
	const FDashAction* Act = (S.Act == EAct::DashThen || S.Act == EAct::HoldThenDash) ? &DashActionAt(S.Arg) : nullptr;
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
	const FDashAction* Act = (S.Act == EAct::DashThen || S.Act == EAct::HoldThenDash) ? &DashActionAt(S.Arg) : nullptr;
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
		ActionDuringDash, ActionAfterDash, DashMeleePressFrame, DashMeleeStartFrame, S.Act == EAct::MeleeDash ? DashFoeHpAtStep - HealthOf(Foe.Get()) : 0.f, DashMagAtBegin, ActiveMagazine(),
		M ? (int32)M->MovementMode.GetValue() : -1);
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
	for (int32 i = 0; i < PendingShots.Num(); ++i)
	{
		if (Frame < PendingShots[i].Key) continue;
		const FString Path = FPaths::ProjectSavedDir() / TEXT("MOB") / FString::Printf(TEXT("cprobe_%s_%s.png"), *Tag, *PendingShots[i].Value);
		FScreenshotRequest::RequestScreenshot(Path, true, false);
		UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_SHOT frame=%d t=%.4f step=%s path=%s"), Frame, Now, StepName, *Path);
		PendingShots.RemoveAt(i);
		break;
	}

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

void UMOBContentProbeSubsystem::Finish(bool bOk)
{
	bDone = true;
	if (FailMsgHandle.IsValid()) FailMsgHandle.Unregister();
	LogBodies(TEXT("end"));
	const FString Path = FPaths::ProjectSavedDir() / TEXT("MOB") / FString::Printf(TEXT("cprobe_%s.csv"), *Tag);
	const bool bSaved = FFileHelper::SaveStringToFile(Csv, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogMOBCProbe, Log, TEXT("MOB_CPROBE_DONE ok=%d saved=%d path=%s frames=%d steps=%d realSec=%.1f"),
		bOk ? 1 : 0, bSaved ? 1 : 0, *Path, Frame, FMath::Max(Step, 0), FPlatformTime::Seconds() - StartRealAt);
	FPlatformMisc::RequestExit(false, TEXT("MOBContentProbe"));
}
