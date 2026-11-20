#include "MOBSimpleAnimInstance.h"

#include "Animation/AnimNodeBase.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotify_PlaySound.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimMontageEvaluationState.h"
#include "Animation/AnimNotifyQueue.h"
#include "Animation/BlendProfile.h"
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
#include "Engine/SkeletalMesh.h"
#include "Cosmetics/LyraCharacterPartTypes.h"

#include "AnimClips.h"
#include "AnimPose.h"
#include "GameProtocol.h"
#include "Weapon.h"

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
	const TCHAR* ClipName(bool bAimJump, const TCHAR* const* WeaponClipNames, int32 I)
	{
		return bAimJump ? WeaponClipNames[I] : CLIP_NAMES[I];
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

	// 무기별 클립 이름(9단계) — -MOBSimpleAimJump 의 30자리에 무기마다 무엇을 넣나. 권총 이름은 Lyra 원본 기록(9-1, animcmp_{ss,aj,run}_pistol_probe.csv)의
	// 재생 칸에서 읽었다(조준 세 자세는 파일 이름 기준 — 뽑을 때 MOB_SIMPLE_AO sample= 로그로 조준 보정 자산 표본과 맞는지 본다).
	// 샷건 몸 = 소총 몸이라 소총 이름을 그대로 쓴다(D45 — 출발 · 멈춤 경로 22부위 차이 0.42cm, 같은 소총 기록끼리 0.68~0.76cm). 앞의 5개만 쓰는 옛 모드(CLIP_NAMES)는 소총뿐이다
	const TCHAR* const RIFLE_FOLDER = TEXT("/Game/Characters/Heroes/Mannequin/Animations/Locomotion/Rifle");
	const TCHAR* const PISTOL_FOLDER = TEXT("/Game/Characters/Heroes/Mannequin/Animations/Locomotion/Pistol");
	const TCHAR* const PISTOL_CLIP_NAMES[FMOBSimpleAnimProxy::kClips] = {
		TEXT("MF_Pistol_Idle_Hipfire"),
		TEXT("MF_Pistol_Jog_Fwd"),
		TEXT("MF_Pistol_Jog_Bwd"),
		TEXT("MF_Pistol_Jog_Left"),
		TEXT("MF_Pistol_Jog_Right"),
	};
	const TCHAR* const PISTOL_AIM_NAMES[FMOBSimpleAnimProxy::kAim] = {
		TEXT("MF_Pistol_Idle_ADS_AO_CU"),
		TEXT("MF_Pistol_Idle_ADS_AO_CC"),
		TEXT("MF_Pistol_Idle_ADS_AO_CD"),
	};
	const TCHAR* const PISTOL_JUMP_NAMES[FMOBSimpleAnimProxy::kJump] = {
		TEXT("MM_Pistol_Jump_Start"),
		TEXT("MM_Pistol_Jump_Start_Loop"),
		TEXT("MM_Pistol_Jump_Apex"),
		TEXT("MM_Pistol_Jump_Fall_Loop"),
		TEXT("MM_Pistol_Jump_Fall_Land"),
	};
	struct FWeaponClipNames
	{
		const TCHAR* Key;					// -MOBACWeapon 값 = 클립 파일의 묶음 이름
		const TCHAR* Folder;				// 걸음 · 점프 · 전환 · 착지 충격 · 대기 몸짓 클립이 든 폴더
		const TCHAR* const* ClipNames;		// kClips 개(서기 · 달리기 넷)
		const TCHAR* Prefix;				// 전환 클립 이름 앞부분(MF_Rifle_Jog_Fwd_Start 의 MF_Rifle)
		const TCHAR* AimOffsetPath;
		const TCHAR* const* AimNames;		// kAim 개
		const TCHAR* const* JumpNames;		// kJump 개
		const TCHAR* Recovery;
		const TCHAR* Fidget;
	};
	const FWeaponClipNames WEAPON_CLIPS[] = {
		{ TEXT("rifle"), RIFLE_FOLDER, CLIP_NAMES_FEMININE, TEXT("MF_Rifle"), AIM_OFFSET_PATH, AIM_NAMES, JUMP_NAMES, RECOVERY_NAME, FIDGET_NAME },
		{ TEXT("pistol"), PISTOL_FOLDER, PISTOL_CLIP_NAMES, TEXT("MF_Pistol"),
			TEXT("/Game/Characters/Heroes/Mannequin/Animations/AimOffsets/AO_MF_Pistol_Idle_ADS.AO_MF_Pistol_Idle_ADS"), PISTOL_AIM_NAMES, PISTOL_JUMP_NAMES,
			TEXT("MM_Pistol_Jump_RecoveryAdditive"), TEXT("MF_Pistol_IdleBreak_Scan") },
		{ TEXT("shotgun"), RIFLE_FOLDER, CLIP_NAMES_FEMININE, TEXT("MF_Rifle"), AIM_OFFSET_PATH, AIM_NAMES, JUMP_NAMES, RECOVERY_NAME, FIDGET_NAME },
	};

	// 없거나 모르는 값이면 소총 — 기록 도구(MOBAnimCompareSubsystem)의 기본 무기와 같다
	const FWeaponClipNames& WeaponClipsFromCommandLine()
	{
		FString Key;
		FParse::Value(FCommandLine::Get(), TEXT("MOBACWeapon="), Key);
		for (const FWeaponClipNames& Weapon : WEAPON_CLIPS)
		{
			if (Key == Weapon.Key) return Weapon;
		}
		return WEAPON_CLIPS[0];
	}

	// 무기별 몽타주(9단계 3-a) — 서버 몽타주 표의 입력. 자산 이름은 Lyra 원본 기록(9단계 1-b, animcmp_ev_<무기>_montages.csv)에서 그 무기를 든 채 사건을 냈을 때 뜬 것이다
	// (사격 = fire_stand · 재장전 = reload · 꺼내기 = 그 무기로 돌아온 swap_back 조각의 첫 프레임). 차례 = Shared/AnimClips.h EMontageKind(사격 · 재장전 · 꺼내기 · 빼기 · 부활).
	// 샷건 꺼내기 = 소총 것(샷건 자산이 없고 기록에서도 AM_MM_Rifle_Equip) · 빼기 · 부활은 세 무기가 같은 자산이다
	// 근접 · 맞은 쪽 반응 뒤 · 앞(컨텐츠 E-2) — 근접 자산은 근접 시험판 e1g3 에서 그 무기를 든 채 능력이 튼 것(실측), 반응 둘은 근접에 늘 나오는 두 개(D58, 세 무기 같은 자산)
	// 대시 앞 · 뒤 · 왼 · 오른(컨텐츠 D-4) — GA_Hero_Dash 가 트는 넷, 세 무기 같은 자산(D-0 dump_dash.txt, 원본 기준)
	// 수류탄 던짐(컨텐츠 G-4) — GA_Grenade 가 트는 하나, 세 무기 같은 자산(능력 CDO 에 하나 — G-0 dump_gren.txt, 원본 기준)
	constexpr int32 kMontageKinds = 13;
	const TCHAR* const UNEQUIP_MONTAGE = TEXT("/Game/Characters/Heroes/Mannequin/Animations/Actions/AM_MM_Generic_Unequip.AM_MM_Generic_Unequip");
	const TCHAR* const SPAWN_MONTAGE = TEXT("/Game/Characters/Heroes/Mannequin/Animations/Actions/AM_MM_Pistol_Spawn.AM_MM_Pistol_Spawn");
	const TCHAR* const RIFLE_EQUIP_MONTAGE = TEXT("/Game/Weapons/Rifle/Animations/AM_MM_Rifle_Equip.AM_MM_Rifle_Equip");
	const TCHAR* const HIT_BACK_MONTAGE = TEXT("/Game/Characters/Heroes/Mannequin/Animations/Actions/AM_MM_HitReact_Back_Med_01.AM_MM_HitReact_Back_Med_01");
	const TCHAR* const HIT_FRONT_MONTAGE = TEXT("/Game/Characters/Heroes/Mannequin/Animations/Actions/AM_MM_HitReact_Front_Med_02.AM_MM_HitReact_Front_Med_02");
	const TCHAR* const DASH_FORWARD_MONTAGE = TEXT("/Game/Characters/Heroes/Mannequin/Animations/Actions/AM_MM_Dash_Forward.AM_MM_Dash_Forward");
	const TCHAR* const DASH_BACKWARD_MONTAGE = TEXT("/Game/Characters/Heroes/Mannequin/Animations/Actions/AM_MM_Dash_Backward.AM_MM_Dash_Backward");
	const TCHAR* const DASH_LEFT_MONTAGE = TEXT("/Game/Characters/Heroes/Mannequin/Animations/Actions/AM_MM_Dash_Left.AM_MM_Dash_Left");
	const TCHAR* const DASH_RIGHT_MONTAGE = TEXT("/Game/Characters/Heroes/Mannequin/Animations/Actions/AM_MM_Dash_Right.AM_MM_Dash_Right");
	const TCHAR* const GRENADE_MONTAGE = TEXT("/Game/Characters/Heroes/Mannequin/Animations/Actions/AM_MM_Rifle_GrenadeToss.AM_MM_Rifle_GrenadeToss");
	struct FWeaponMontageNames
	{
		const TCHAR* Key;							// FWeaponClipNames::Key 와 같다
		const TCHAR* Montages[kMontageKinds];
		const TCHAR* OverridePose;					// 쏜 뒤 윗몸 덮기 자세(시각 0 고정). 1-c 기록에서 덮기 비중과 함께 움직인 클립
	};
	const FWeaponMontageNames WEAPON_MONTAGES[] = {
		{ TEXT("rifle"), { TEXT("/Game/Weapons/Rifle/Animations/AM_MM_Rifle_Fire.AM_MM_Rifle_Fire"), TEXT("/Game/Weapons/Rifle/Animations/AM_MM_Rifle_Reload.AM_MM_Rifle_Reload"),
			RIFLE_EQUIP_MONTAGE, UNEQUIP_MONTAGE, SPAWN_MONTAGE,
			TEXT("/Game/Characters/Heroes/Mannequin/Animations/Actions/AM_MM_Rifle_Melee.AM_MM_Rifle_Melee"), HIT_BACK_MONTAGE, HIT_FRONT_MONTAGE,
			DASH_FORWARD_MONTAGE, DASH_BACKWARD_MONTAGE, DASH_LEFT_MONTAGE, DASH_RIGHT_MONTAGE, GRENADE_MONTAGE },
			TEXT("/Game/Characters/Heroes/Mannequin/Animations/Locomotion/Rifle/MF_Rifle_Idle_Hipfire.MF_Rifle_Idle_Hipfire") },
		{ TEXT("pistol"), { TEXT("/Game/Weapons/Pistol/Animations/AM_MM_Pistol_Fire.AM_MM_Pistol_Fire"), TEXT("/Game/Weapons/Pistol/Animations/AM_MM_Pistol_Reload.AM_MM_Pistol_Reload"),
			TEXT("/Game/Weapons/Pistol/Animations/AM_MM_Pistol_Equip.AM_MM_Pistol_Equip"), UNEQUIP_MONTAGE, SPAWN_MONTAGE,
			TEXT("/Game/Characters/Heroes/Mannequin/Animations/Actions/AM_MM_Pistol_Melee.AM_MM_Pistol_Melee"), HIT_BACK_MONTAGE, HIT_FRONT_MONTAGE,
			DASH_FORWARD_MONTAGE, DASH_BACKWARD_MONTAGE, DASH_LEFT_MONTAGE, DASH_RIGHT_MONTAGE, GRENADE_MONTAGE },
			TEXT("/Game/Characters/Heroes/Mannequin/Animations/Locomotion/Pistol/MF_Pistol_Hipfire_OverridePose.MF_Pistol_Hipfire_OverridePose") },
		{ TEXT("shotgun"), { TEXT("/Game/Weapons/Shotgun/Animations/AM_MM_Shotgun_Fire.AM_MM_Shotgun_Fire"), TEXT("/Game/Weapons/Shotgun/Animations/AM_MM_Shotgun_Reload.AM_MM_Shotgun_Reload"),
			RIFLE_EQUIP_MONTAGE, UNEQUIP_MONTAGE, SPAWN_MONTAGE,
			TEXT("/Game/Characters/Heroes/Mannequin/Animations/Actions/AM_MM_Shotgun_Melee.AM_MM_Shotgun_Melee"), HIT_BACK_MONTAGE, HIT_FRONT_MONTAGE,
			DASH_FORWARD_MONTAGE, DASH_BACKWARD_MONTAGE, DASH_LEFT_MONTAGE, DASH_RIGHT_MONTAGE, GRENADE_MONTAGE },
			TEXT("/Game/Characters/Heroes/Mannequin/Animations/Locomotion/Rifle/MF_Rifle_Idle_Hipfire.MF_Rifle_Idle_Hipfire") },
	};
	constexpr int32 MONTAGE_CURVE_HZ = 120;		// 몽타주 · 몽타주 클립 곡선을 뽑는 간격(클립 표본과 같은 간격, 제안값)

	// 조준 클립(컨텐츠 C-3, D60) — 클립 파일 버전 3 에서 30개 뒤에 붙는 17개. 차례 = Shared/AnimClips.h ACLIP_ADS_IDLE 부터
	// 서기 · 걷기 넷(앞 · 뒤 · 왼 · 오른) · 전환 12(출발 · 멈춤 · 돌아서기 × 같은 방향 차례). 뽑을 때만 읽는다(재생은 서버가 정한 자세를 받는다).
	// 이름 = Lyra 여성 몸 무기 층 기본값(ABP_RifleAnimLayers_Feminine · ABP_PistolAnimLayers_Feminine CDO)에 든 자산(원본 기준). 권총은 MF · MM 이 섞여 있다.
	// 샷건 = 소총 것(D45 — 샷건 층도 소총 몸 클립을 쓴다)
	constexpr int32 kAdsClips = 17;
	const TCHAR* const RIFLE_ADS_NAMES[kAdsClips] = {
		TEXT("MF_Rifle_Idle_ADS"), TEXT("MF_Rifle_Walk_Fwd"), TEXT("MF_Rifle_Walk_Bwd"), TEXT("MF_Rifle_Walk_Left"), TEXT("MF_Rifle_Walk_Right"),
		TEXT("MF_Rifle_Walk_Fwd_Start"), TEXT("MF_Rifle_Walk_Bwd_Start"), TEXT("MF_Rifle_Walk_Left_Start"), TEXT("MF_Rifle_Walk_Right_Start"),
		TEXT("MF_Rifle_Walk_Fwd_Stop"), TEXT("MF_Rifle_Walk_Bwd_Stop"), TEXT("MF_Rifle_Walk_Left_Stop"), TEXT("MF_Rifle_Walk_Right_Stop"),
		TEXT("MF_Rifle_Walk_Fwd_Pivot"), TEXT("MF_Rifle_Walk_Bwd_Pivot"), TEXT("MF_Rifle_Walk_Left_Pivot"), TEXT("MF_Rifle_Walk_Right_Pivot"),
	};
	const TCHAR* const PISTOL_ADS_NAMES[kAdsClips] = {
		TEXT("MF_Pistol_Idle_ADS"), TEXT("MF_Pistol_Walk_Fwd"), TEXT("MF_Pistol_Walk_Bwd"), TEXT("MF_Pistol_Walk_Left"), TEXT("MF_Pistol_Walk_Right"),
		TEXT("MF_Pistol_Walk_Fwd_Start"), TEXT("MF_Pistol_Walk_Bwd_Start"), TEXT("MM_Pistol_Walk_Left_Start"), TEXT("MF_Pistol_Walk_Right_Start"),
		TEXT("MF_Pistol_Walk_Fwd_Stop"), TEXT("MF_Pistol_Walk_Bwd_Stop"), TEXT("MF_Pistol_Walk_Left_Stop"), TEXT("MF_Pistol_Walk_Right_Stop"),
		TEXT("MM_Pistol_Walk_Fwd_Pivot"), TEXT("MM_Pistol_Walk_Bwd_Pivot"), TEXT("MM_Pistol_Walk_Left_Pivot"), TEXT("MM_Pistol_Walk_Right_Pivot"),
	};
	struct FWeaponAdsNames
	{
		const TCHAR* Key;			// FWeaponClipNames::Key 와 같다
		const TCHAR* Folder;
		const TCHAR* const* Names;	// kAdsClips 개
	};
	const FWeaponAdsNames WEAPON_ADS[] = {
		{ TEXT("rifle"), RIFLE_FOLDER, RIFLE_ADS_NAMES },
		{ TEXT("pistol"), PISTOL_FOLDER, PISTOL_ADS_NAMES },
		{ TEXT("shotgun"), RIFLE_FOLDER, RIFLE_ADS_NAMES },
	};

	// 앉기 클립(컨텐츠 D-3) — 클립 파일 버전 4 에서 조준 17개 뒤(47~66)에 붙는 20개. 차례 = Shared/AnimClips.h ACLIP_CROUCH_IDLE 부터
	// 서기 · 걷기 넷 · 전환 12(조준 클립과 같은 차례) · 서기 안 앉기 · 일어서기 전환(Crouch_Idle_Entry · _Exit) · 앉아 쏜 뒤 덮기 자세(Aim_HipFirePose_Crouch).
	// 이름 = Lyra 여성 몸 무기 층 기본값(ABP_RifleAnimLayers_Feminine · ABP_PistolAnimLayers_Feminine CDO)에 든 자산(원본 기준) — 두 무기 다 MM 이다.
	// 샷건 = 소총 것(샷건 여성 층은 값을 안 덮고, 샷건 층 기본값이 소총 앉기 클립이다). 앉은 제자리 돌기(Crouch_TurnInPlace_*)는 허리 쪽도 안 옮겨 뺐다
	constexpr int32 kCrouchClips = 20;
	const TCHAR* const RIFLE_CROUCH_NAMES[kCrouchClips] = {
		TEXT("MM_Rifle_Crouch_Idle"), TEXT("MM_Rifle_Crouch_Walk_Fwd"), TEXT("MM_Rifle_Crouch_Walk_Bwd"), TEXT("MM_Rifle_Crouch_Walk_Left"), TEXT("MM_Rifle_Crouch_Walk_Right"),
		TEXT("MM_Rifle_Crouch_Walk_Fwd_Start"), TEXT("MM_Rifle_Crouch_Walk_Bwd_Start"), TEXT("MM_Rifle_Crouch_Walk_Left_Start"), TEXT("MM_Rifle_Crouch_Walk_Right_Start"),
		TEXT("MM_Rifle_Crouch_Walk_Fwd_Stop"), TEXT("MM_Rifle_Crouch_Walk_Bwd_Stop"), TEXT("MM_Rifle_Crouch_Walk_Left_Stop"), TEXT("MM_Rifle_Crouch_Walk_Right_Stop"),
		TEXT("MM_Rifle_Crouch_Walk_Fwd_Pivot"), TEXT("MM_Rifle_Crouch_Walk_Bwd_Pivot"), TEXT("MM_Rifle_Crouch_Walk_Left_Pivot"), TEXT("MM_Rifle_Crouch_Walk_Right_Pivot"),
		TEXT("MM_Rifle_Crouch_Entry"), TEXT("MM_Rifle_Crouch_Exit"), TEXT("MM_Rifle_Crouch_OverridePose"),
	};
	const TCHAR* const PISTOL_CROUCH_NAMES[kCrouchClips] = {
		TEXT("MM_Pistol_Crouch_Idle"), TEXT("MM_Pistol_Crouch_Walk_Fwd"), TEXT("MM_Pistol_Crouch_Walk_Bwd"), TEXT("MM_Pistol_Crouch_Walk_Left"), TEXT("MM_Pistol_Crouch_Walk_Right"),
		TEXT("MM_Pistol_Crouch_Walk_Fwd_Start"), TEXT("MM_Pistol_Crouch_Walk_Bwd_Start"), TEXT("MM_Pistol_Crouch_Walk_Left_Start"), TEXT("MM_Pistol_Crouch_Walk_Right_Start"),
		TEXT("MM_Pistol_Crouch_Walk_Fwd_Stop"), TEXT("MM_Pistol_Crouch_Walk_Bwd_Stop"), TEXT("MM_Pistol_Crouch_Walk_Left_Stop"), TEXT("MM_Pistol_Crouch_Walk_Right_Stop"),
		TEXT("MM_Pistol_Crouch_Walk_Fwd_Pivot"), TEXT("MM_Pistol_Crouch_Walk_Bwd_Pivot"), TEXT("MM_Pistol_Crouch_Walk_Left_Pivot"), TEXT("MM_Pistol_Crouch_Walk_Right_Pivot"),
		TEXT("MM_Pistol_Crouch_Entry"), TEXT("MM_Pistol_Crouch_Exit"), TEXT("MM_Pistol_Crouch_OverridePose"),
	};
	const FWeaponAdsNames WEAPON_CROUCH[] = {
		{ TEXT("rifle"), RIFLE_FOLDER, RIFLE_CROUCH_NAMES },
		{ TEXT("pistol"), PISTOL_FOLDER, PISTOL_CROUCH_NAMES },
		{ TEXT("shotgun"), RIFLE_FOLDER, RIFLE_CROUCH_NAMES },
	};

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

	// 출발 · 멈춤 · 돌아서기(-MOBSimpleTransitions) — Lyra 원본 실행값에서 읽은 값(animcmp_ssp_lyra_probe.csv, 11-10 · 60프레임 · 네 방향).
	// 넘어가는 때는 그 순간 나가는 클립의 시각으로 적었다. 네 칸의 차례 = 앞 · 뒤 · 왼쪽 · 오른쪽
	enum ETransitionKind { TRANSITION_START, TRANSITION_STOP, TRANSITION_PIVOT };
	enum EDirection { DIR_FWD, DIR_BWD, DIR_LEFT, DIR_RIGHT };
	const TCHAR* const TRANSITION_KINDS[] = { TEXT("Start"), TEXT("Stop"), TEXT("Pivot") };
	const TCHAR* const DIRECTION_NAMES[] = { TEXT("Fwd"), TEXT("Bwd"), TEXT("Left"), TEXT("Right") };
	const FName TRANSITION_DISTANCE_CURVE(TEXT("Distance"));	// 클립마다 든 거리 곡선. 시작 · 멈춤 지점이 0 이고 그전은 음수다
	constexpr int32 TRANSITION_TABLE_HZ = 120;
	constexpr float CARDINAL_DEAD_ZONE = 10.f;		// 네 방향 나눌 때 앞 · 뒤를 넓히는 각. 확인: Lyra 변수 CardinalDirectionDeadZone = 10, 45도 대각선 출발이 앞 클립
	// 섞는 시간 = 상태가 바뀐 뒤 새 클립 몫의 프레임별 값에 Smooth 곡선을 최소 제곱으로 맞춘 값(앞뒤 조합마다 1~11회, 평균 오차는 아래 적었다)
	constexpr float TO_START_SECONDS = 0.21f;		// 서기 → 출발. 오차 0.044 — 끝자락이 곡선보다 길게 늘어진다
	constexpr float STOP_TO_START_SECONDS = 0.175f;	// 멈춤 → 출발. 오차 0.069 — 위와 같은 끝자락
	constexpr float TO_STOP_SECONDS = 0.28f;		// 달리기 · 돌아서기 → 멈춤. 맞춘 값 0.26 + 시작이 1프레임 늦음(오차 0.020). 단순 계산은 늦게 시작하는 칸이 없어 합친 길이에 가깝게 잡았다
	constexpr float TO_PIVOT_SECONDS = 0.25f;		// 달리기 · 출발 → 돌아서기. 오차 0.003
	constexpr float START_TO_CYCLE_SECONDS = 0.99f;	// 출발 → 달리기. 오차 0.004
	constexpr float PIVOT_TO_CYCLE_SECONDS = 0.5f;	// 돌아서기 → 달리기. 오차 0.003
	constexpr float STOP_TO_IDLE_SECONDS = 0.74f;	// 멈춤 → 서기. 오차 0.003
	constexpr float START_EXIT_TIMES[] = { 0.791f, 1.165f, 1.185f, 1.122f };	// 출발 클립이 이 시각을 지나면 달리기로. 왜 이 값인지는 모른다(애니 블루프린트를 안 열었다) — 판을 바꿔도 같았다
	constexpr float STOP_EXIT_TIMES[] = { 1.712f, 2.046f, 2.180f, 2.277f };		// 멈춤 클립이 이 시각을 지나면 서기로
	constexpr float PIVOT_EXIT_TIMES[] = { 1.344f, 1.591f, 1.565f, 1.358f };	// 돌아서기 클립이 이 시각을 지나면 달리기로. 클립의 TransitionToLocomotion 표시 시작(1.325 · 1.572 · 1.543 · 1.335) 바로 다음 프레임이다
	constexpr float STEP_LEADER_WEIGHT = 0.5f;		// 나가는 전환 클립의 몫이 이보다 크면 달리기 클립이 그 발을 따른다(언리얼 동기화 그룹은 가장 무거운 클립이 앞장선다)
	constexpr float PIVOT_MIN_SPEED = 10.f;			// 이보다 느리면 돌아설 것 없이 출발로 본다(제안값)
	constexpr float DISTANCE_RATE_MIN = 0.25f;		// 거리로 진행할 때 재생 속도 범위. 확인: 돌아선 직후 0.42배까지 내려감 · 출발 첫 프레임 2.4배
	constexpr float DISTANCE_RATE_MAX = 5.f;

	FString TransitionName(const TCHAR* Prefix, int32 Kind, int32 Direction)
	{
		return FString::Printf(TEXT("%s_Jog_%s_%s"), Prefix, DIRECTION_NAMES[Direction], TRANSITION_KINDS[Kind]);
	}

	int32 CardinalFromAngle(float Angle)
	{
		const float Abs = FMath::Abs(Angle);
		if (Abs <= 45.f + CARDINAL_DEAD_ZONE) return DIR_FWD;
		if (Abs >= 135.f - CARDINAL_DEAD_ZONE) return DIR_BWD;
		return Angle > 0.f ? DIR_RIGHT : DIR_LEFT;
	}

	float AngleOf(const FVector& V)
	{
		return FMath::RadiansToDegrees(FMath::Atan2((float)V.Y, (float)V.X));
	}

	// 지금 속도로 멈출 때까지 갈 거리 — 엔진 PredictGroundMovementStopLocation 과 같은 식. 입력은 속도와 이동 상수뿐이라 서버도 같은 값을 낸다
	float PredictStopDistance(float Speed, const UCharacterMovementComponent* Move)
	{
		if (!Move) return 0.f;
		const float Friction = FMath::Max(0.f, (Move->bUseSeparateBrakingFriction ? Move->BrakingFriction : Move->GroundFriction) * FMath::Max(0.f, Move->BrakingFrictionFactor));
		const float Braking = FMath::Max(0.f, Move->BrakingDecelerationWalking);
		const float Divisor = Friction * Speed + Braking;
		if (Divisor <= 0.f) return 0.f;
		const float Time = Speed / Divisor;
		return FMath::Max(0.f, Speed * Time + 0.5f * (-Friction * Speed - Braking) * Time * Time);
	}

	// 방향이 바뀌는 지점까지 거리 — 엔진 PredictGroundMovementPivotLocation 과 같은 식. 입력은 가속 · 속도 · 지면 마찰뿐이다
	float PredictPivotDistance(const FVector& Acceleration, const FVector& Velocity, float GroundFriction)
	{
		const FVector Acceleration2D(Acceleration.X, Acceleration.Y, 0.0);
		FVector Direction;
		float Size = 0.f;
		Acceleration2D.ToDirectionAndLength(Direction, Size);
		const float Along = (float)(Velocity | Direction);
		if (Along >= 0.f) return 0.f;
		const float SpeedAlong = -Along;
		const float Time = SpeedAlong / (Size + 2.f * SpeedAlong * GroundFriction);
		const FVector Force = Acceleration - (Velocity - Direction * Velocity.Size2D()) * GroundFriction;
		return (float)(Velocity * Time + 0.5 * Force * Time * Time).Size2D();
	}

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
	const bool bServerAnim = FParse::Param(FCommandLine::Get(), TEXT("MOBServerAnim"));	// 받은 상태의 클립 번호는 전환 클립까지 가리키므로 전부 읽는다
	bAimJump = bServerAnim || FParse::Param(FCommandLine::Get(), TEXT("MOBSimpleAimJump"));
	bTransitions = bServerAnim || (bAimJump && FParse::Param(FCommandLine::Get(), TEXT("MOBSimpleTransitions")));
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

	const FWeaponClipNames& Primary = bAimJump ? WeaponClipsFromCommandLine() : WEAPON_CLIPS[0];
	WeaponKey = Primary.Key;

	// 읽을 무기 — 서버 구동이면 서버 무기 번호(EWeapon) 차례로 전부(무기 이름 = 클립 이름 표의 Key 로 잇는다), 아니면 명령줄 무기 하나
	TArray<const FWeaponClipNames*, TInlineAllocator<4>> Wanted;
	if (bServerAnim)
	{
		for (int32 W = 0; W < (int32)mo::EWeapon::Count; ++W)
		{
			const FString Name = UTF8_TO_TCHAR(mo::GetWeaponDef((mo::EWeapon)W).name);
			const FWeaponClipNames* Found = nullptr;
			for (const FWeaponClipNames& Names : WEAPON_CLIPS)
			{
				if (Name == Names.Key) Found = &Names;
			}
			if (!Found) UE_LOG(LogMOBSimple, Error, TEXT("MOB_SIMPLE_FAIL weapon=%d name=%s — 클립 이름 표에 없어 이 무기 한 벌을 못 읽는다"), W, *Name);
			if (Found == &Primary) PrimarySet = W;
			Wanted.Add(Found);
		}
	}
	else
	{
		Wanted.Add(&Primary);
	}

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

	for (const FWeaponClipNames* Weapon : Wanted)
	{
		FMOBLoadedClipSet& Set = LoadedSets.AddDefaulted_GetRef();
		if (!Weapon) continue;
		Set.Key = Weapon->Key;
		UE_LOG(LogMOBSimple, Log, TEXT("MOB_SIMPLE_WEAPON key=%s folder=%s set=%d"), Weapon->Key, Weapon->Folder, LoadedSets.Num() - 1);
		for (int32 I = 0; I < FMOBSimpleAnimProxy::kClips; ++I) Set.Clips.Add(Load(Weapon->Folder, ClipName(bAimJump, Weapon->ClipNames, I), TEXT("LOAD")));
		if (!bAimJump) continue;
		for (int32 I = 0; I < FMOBSimpleAnimProxy::kAim; ++I) Set.Aim.Add(Load(AIM_FOLDER, Weapon->AimNames[I], TEXT("AIM")));
		for (int32 I = 0; I < FMOBSimpleAnimProxy::kJump; ++I) Set.Jump.Add(Load(Weapon->Folder, Weapon->JumpNames[I], TEXT("JUMP")));
		for (int32 I = 0; I < FMOBSimpleAnimProxy::kTransitions && bTransitions; ++I) Set.Jump.Add(Load(Weapon->Folder, *TransitionName(Weapon->Prefix, I / 4, I % 4), TEXT("LOAD")));
		Set.Recovery = Load(Weapon->Folder, Weapon->Recovery, TEXT("JUMP"));
		Set.Fidget = Load(Weapon->Folder, Weapon->Fidget, TEXT("IDLE"));
		ReadPitches(Weapon->AimOffsetPath, Set.Aim, Weapon->AimNames, Set.AimPitches);
		if (!bServerAnim) continue;

		// 몽타주(9단계 5-a) — 받은 몽타주 칸을 언리얼 경로(손가락 · 꼬임 뼈)에 얹을 자산. 차례 = 종류(EMontageKind), 자산 = 클립 파일을 뽑은 표(WEAPON_MONTAGES) 그대로
		const FWeaponMontageNames* MontageNames = nullptr;
		for (const FWeaponMontageNames& Names : WEAPON_MONTAGES)
		{
			if (FCString::Strcmp(Names.Key, Weapon->Key) == 0) MontageNames = &Names;
		}
		if (!MontageNames) UE_LOG(LogMOBSimple, Error, TEXT("MOB_SIMPLE_FAIL montages weapon=%s — 몽타주 이름 표에 없다"), Weapon->Key);
		int32 MontagesLoaded = 0;
		for (int32 K = 0; MontageNames && K < kMontageKinds; ++K)
		{
			UAnimMontage* Montage = LoadObject<UAnimMontage>(nullptr, MontageNames->Montages[K]);
			if (!Montage) UE_LOG(LogMOBSimple, Error, TEXT("MOB_SIMPLE_FAIL montage=%s"), MontageNames->Montages[K]);
			MontagesLoaded += Montage ? 1 : 0;
			Set.Montages.Add(Montage);
		}
		UE_LOG(LogMOBSimple, Log, TEXT("MOB_SIMPLE_MONTAGES key=%s loaded=%d of=%d"), Weapon->Key, MontagesLoaded, kMontageKinds);
	}
	if (!bAimJump) return;
	for (int32 I = 0; I < FMOBSimpleAnimProxy::kAim; ++I) LoadedReadyAim.Add(Load(AIM_FOLDER, READY_AIM_NAMES[I], TEXT("AIM")));
	ReadPitches(READY_AIM_OFFSET_PATH, LoadedReadyAim, READY_AIM_NAMES, ReadyAimPitches);
}

void UMOBSimpleAnimInstance::ResetPlayback()
{
	GetProxyOnGameThread<FMOBSimpleAnimProxy>().ResetPlayback();
}

void FMOBSimpleAnimProxy::ResetPlayback()
{
	++ResetGeneration;
	MoveSeq = 0;
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
	MoveState = EMoveState::Idle;
	MoveDirection = 0;
	PivotAccelDirection = FVector2D::ZeroVector;
	bBeforePivot = false;
	FollowClip = INDEX_NONE;
}

// 받은 상태를 언리얼 경로의 재생 상태로 옮기고(손가락 · 꼬임 뼈용), 판정 뼈에 덮을 자세를 받아 둔다
void FMOBSimpleAnimProxy::SetServerFrame(const mo::AnimPoseInput& Input, int32 Weapon, const int32* MontageWeapons, const mo::BoneTransform* Local, int32 Count, const TArray<FName>& BoneNames, FMOBServerAnimResult& Out)
{
	bServerDriven = true;
	// 받은 클립 번호 · 시각은 그 무기 한 벌의 것이다 — 한 벌이 다르면 먼저 갈아 넣는다. 아직 안 묶었으면(첫 프레임) 묶을 때 이 한 벌부터 넣는다
	WantedSet = Weapon;
	if (ClipSets.Num() > 0 && Weapon != ActiveSet)
	{
		if (ClipSets.IsValidIndex(Weapon) && ClipSets[Weapon].bLoaded)
		{
			UseSet(ClipSets[Weapon]);
			ActiveSet = Weapon;
			Out.SetChange = 1;
		}
		else
		{
			Out.SetChange = -1;
		}
	}

	// 몽타주 칸 — 칸마다 그 칸 무기 한 벌의 자산을 쓴다(무기를 바꾸는 틱엔 앞 무기 「빼기」와 새 무기 「꺼내기」가 함께 돈다, D51).
	// 알림은 여기서 안 튼다 — 이 함수엔 보여 줄 상태만 오므로, 받은 상태를 하나씩 넣는 StepMontages 가 따로 훑는다
	MontageCellCount = 0;
	for (int32 M = 0; M < Input.montageCount && M < mo::kAnimMontageMax; ++M)
	{
		const mo::AnimMontageSlot& View = Input.montages[M];
		const int32 CellWeapon = MontageWeapons ? MontageWeapons[M] : INDEX_NONE;
		UAnimMontage* Montage = ClipSets.IsValidIndex(CellWeapon) && View.kind < kMontageKindCount ? ClipSets[CellWeapon].Montages[View.kind] : nullptr;
		if (!Montage)
		{
			++Out.MontageMisses;
			continue;
		}
		FMontageCell& Cell = MontageCells[MontageCellCount++];
		Cell.Montage = Montage;
		Cell.Position = View.position;
		Cell.Weight = View.weight;
	}
	Out.MontageCells = MontageCellCount;
	for (int32 I = 0; I < kClips; ++I)
	{
		Weights[I] = Input.weights[I];
		Times[I] = Input.times[I];
	}
	// 받은 쌓기 칸 17~28 은 조준 전환이다(Shared/AnimClips.h StackClipIndex, D60). 이 경로는 조준 클립을 안 묶었으니(JumpClips 는 17칸)
	// 같은 차례의 허리 전환으로 튼다 — 이 경로가 보여 주는 것은 판정 밖 뼈(손가락 · 비틀림 뼈)뿐이고 판정 뼈는 ApplyServerBones 가 덮는다.
	// 그대로 넣으면 JumpClips 밖을 읽는다(C-5 조준 봇 판 `c5vp1` 에서 ApplyJump 가 멈춤)
	// 29~40 앉은 전환도 같은 차례의 허리 전환으로, 41 · 42 서기 안 앉기 · 일어서기 전환은 허리 쪽 짝이 없어 걷기 · 서기 자리로 돌린다(D62 4)
	static_assert(kStackClips == mo::kStackHipClips && kStackClips + kTransitions == mo::kStackAdsEnd && mo::kStackAdsEnd + kTransitions + 2 == mo::kStackClipCount,
		"조준 전환 = 허리 전환 + kTransitions, 앉은 전환 = 조준 전환 + kTransitions, 그 뒤 서기 안 전환 둘 차례여야 한다");
	JumpCount = FMath::Min(Input.layerCount, kJumpStack);
	for (int32 I = 0; I < JumpCount; ++I)
	{
		int32 Clip = Input.layers[I].clip;
		if (Clip >= mo::kStackAdsEnd + kTransitions && Clip < mo::kStackClipCount)
		{
			Clip = JUMP_NONE;
			++Out.StanceLayers;
		}
		else if (Clip >= mo::kStackAdsEnd && Clip < mo::kStackClipCount)
		{
			Clip -= 2 * kTransitions;
			++Out.CrouchLayers;
		}
		else if (Clip >= kStackClips)
		{
			Clip -= kTransitions;
			++Out.AdsLayers;
		}
		if (Clip < JUMP_NONE || Clip >= kStackClips)
		{
			Clip = JUMP_NONE;
			++Out.BadLayers;
		}
		JumpStack[I].Clip = Clip;
		JumpStack[I].Time = Input.layers[I].time;
		JumpStack[I].Fade = Input.layers[I].fade;
		JumpStack[I].FadeSeconds = 0.f;
	}
	FidgetTime = Input.fidgetTime;
	RecoveryTime = Input.recoveryTime;
	AimBlend = Input.aimBlend;
	Pitch = Input.pitch;
	PlantOffset = FVector(Input.plantOffset[0], Input.plantOffset[1], Input.plantOffset[2]);

	if (ServerBoneNames.Num() != Count) ServerBoneNames = BoneNames;
	ServerLocal.SetNumUninitialized(Count, EAllowShrinking::No);
	for (int32 I = 0; I < Count; ++I)
	{
		const mo::BoneTransform& B = Local[I];
		ServerLocal[I] = FTransform(FQuat(B.q[0], B.q[1], B.q[2], B.q[3]), FVector(B.p[0], B.p[1], B.p[2]));
	}
}

// 지난 구간은 앞 상태 칸과 같은 자산 · 앞서지 않은 시각끼리 짝지어 구한다(같은 자산이 둘이면 시각이 가장 가까운 것). 짝이 없으면 새 칸.
// 이어진 칸은 틱 차이만큼 나가 있어야 한다 — 서버 칸은 끝에 닿을 때만 멈춘다(CAnimState::AdvanceMontage, 빼는 중에도 나간다).
// 앞 상태와 틱이 벌어지면(보관한 상태를 넘는 멈춤) 옛 칸이 빠지고 같은 자산의 새 칸이 옛 시각보다 뒤에 서 있을 수 있어, 시각 순서만으론 새 칸을 이어진 칸으로 잘못 짝짓는다.
// 가장 느린 재생 = 0.9(Generic_Unequip 자산 0.9 × 사건 1.0, 쏘기 · 재장전은 무기 값 1.0 ~ 2.2). 0.8 은 그보다 낮춰 잡은 제 판단. 여유 0.001초 = 시각 칸(1/6000초)으로 줄인 두 값의 차이 오차(1/6000초 안)보다 넉넉히 잡은 제 판단
void FMOBSimpleAnimProxy::StepMontages(const mo::AnimPoseInput& Input, uint32 StateTick, const int32* MontageWeapons, bool bContinue, FMOBServerAnimResult& Out,
	TArray<FMontageStep, TInlineAllocator<kMontageCells>>& OutSteps)
{
	constexpr float kMinMontageRate = 0.8f;
	constexpr float kMontagePositionSlack = 0.001f;
	const int32 PreviousCount = bContinue ? NotifyCellCount : 0;
	const int32 TickDelta = PreviousCount > 0 ? (int32)(StateTick - NotifyTick) : 0;
	const float MinAdvance = TickDelta > 0 ? TickDelta * kMinMontageRate / mo::wire::kTickRate - kMontagePositionSlack : 0.f;
	NotifyTick = StateTick;
	FMontageCell Previous[kMontageCells];
	for (int32 I = 0; I < PreviousCount; ++I) Previous[I] = NotifyCells[I];
	bool bPreviousUsed[kMontageCells] = {};
	NotifyCellCount = 0;
	OutSteps.Reset();
	for (int32 M = 0; M < Input.montageCount && M < mo::kAnimMontageMax; ++M)
	{
		const mo::AnimMontageSlot& View = Input.montages[M];
		const int32 CellWeapon = MontageWeapons ? MontageWeapons[M] : INDEX_NONE;
		UAnimMontage* Montage = ClipSets.IsValidIndex(CellWeapon) && View.kind < kMontageKindCount ? ClipSets[CellWeapon].Montages[View.kind] : nullptr;
		if (!Montage) continue;		// 못 넣은 칸은 보여 줄 상태에서 SetServerFrame 이 센다
		FMontageCell& Cell = NotifyCells[NotifyCellCount++];
		Cell.Montage = Montage;
		Cell.Position = View.position;
		Cell.Weight = View.weight;

		int32 Match = INDEX_NONE;
		bool bRestart = false;
		const bool bAtEnd = View.position >= Montage->GetPlayLength() - kMontagePositionSlack;		// 끝에 닿아 멈춘 칸은 덜 나간다
		for (int32 P = 0; P < PreviousCount; ++P)
		{
			if (bPreviousUsed[P] || Previous[P].Montage != Montage || Previous[P].Position > View.position) continue;
			if (!bAtEnd && View.position - Previous[P].Position < MinAdvance)
			{
				bRestart = true;
				continue;
			}
			if (Match == INDEX_NONE || Previous[P].Position > Previous[Match].Position) Match = P;
		}
		if (Match == INDEX_NONE && bRestart) ++Out.MontageRestarts;
		FMontageStep& Passed = OutSteps.AddDefaulted_GetRef();
		Passed.Montage = Montage;
		Passed.To = View.position;
		if (Match != INDEX_NONE)
		{
			bPreviousUsed[Match] = true;
			Passed.From = Previous[Match].Position;
		}
	}
}

namespace
{
	template <typename T, int32 N>
	void CopyItems(T (&To)[N], const T (&From)[N])
	{
		for (int32 I = 0; I < N; ++I) To[I] = From[I];
	}
}

void FMOBSimpleAnimProxy::SaveSet(FClipSet& Out) const
{
	Out.bLoaded = true;
	CopyItems(Out.Clips, Clips);
	CopyItems(Out.Lengths, Lengths);
	CopyItems(Out.RootSpeeds, RootSpeeds);
	CopyItems(Out.StepTimes, StepTimes);
	Out.StepCycle = StepCycle;
	CopyItems(Out.AimClips, AimClips);
	CopyItems(Out.AimPitches, AimPitches);
	Out.AimAdditiveType = AimAdditiveType;
	CopyItems(Out.JumpClips, JumpClips);
	CopyItems(Out.JumpLengths, JumpLengths);
	Out.LandDistances = LandDistances;
	CopyItems(Out.TransitionDistances, TransitionDistances);
	CopyItems(Out.TransitionMarkers, TransitionMarkers);
	Out.RecoveryClip = RecoveryClip;
	Out.RecoveryLength = RecoveryLength;
	Out.RecoveryAdditiveType = RecoveryAdditiveType;
	Out.FidgetClip = FidgetClip;
	Out.FidgetLength = FidgetLength;
}

void FMOBSimpleAnimProxy::UseSet(const FClipSet& In)
{
	CopyItems(Clips, In.Clips);
	CopyItems(Lengths, In.Lengths);
	CopyItems(RootSpeeds, In.RootSpeeds);
	CopyItems(StepTimes, In.StepTimes);
	StepCycle = In.StepCycle;
	CopyItems(AimClips, In.AimClips);
	CopyItems(AimPitches, In.AimPitches);
	AimAdditiveType = In.AimAdditiveType;
	CopyItems(JumpClips, In.JumpClips);
	CopyItems(JumpLengths, In.JumpLengths);
	LandDistances = In.LandDistances;
	CopyItems(TransitionDistances, In.TransitionDistances);
	CopyItems(TransitionMarkers, In.TransitionMarkers);
	RecoveryClip = In.RecoveryClip;
	RecoveryLength = In.RecoveryLength;
	RecoveryAdditiveType = In.RecoveryAdditiveType;
	FidgetClip = In.FidgetClip;
	FidgetLength = In.FidgetLength;
}

// 클립 파일의 뼈를 Shared 결과로 덮는다. 크기는 언리얼 경로 값을 둔다 — Shared 는 크기를 안 다룬다
void FMOBSimpleAnimProxy::ApplyServerBones(FPoseContext& Output)
{
	const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
	if (ServerBoneSerial != Bones.GetSerialNumber() || ServerBoneMap.Num() != ServerBoneNames.Num())
	{
		ServerBoneSerial = Bones.GetSerialNumber();
		ServerBoneMap.Reset();
		int32 Found = 0;
		for (const FName& Name : ServerBoneNames)
		{
			const FCompactPoseBoneIndex Index = FindCompactBone(Bones, *Name.ToString());
			ServerBoneMap.Add(Index.IsValid() ? Index.GetInt() : INDEX_NONE);
			Found += Index.IsValid() ? 1 : 0;
		}
		UE_LOG(LogMOBSimple, Log, TEXT("MOB_SERVERANIM map bones=%d found=%d serial=%d"), ServerBoneNames.Num(), Found, (int32)ServerBoneSerial);
	}
	const int32 Count = FMath::Min(ServerBoneMap.Num(), ServerLocal.Num());
	for (int32 I = 0; I < Count; ++I)
	{
		if (ServerBoneMap[I] == INDEX_NONE) continue;
		FTransform& Bone = Output.Pose[FCompactPoseBoneIndex(ServerBoneMap[I])];
		Bone.SetTranslation(ServerLocal[I].GetTranslation());
		Bone.SetRotation(ServerLocal[I].GetRotation());
	}
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
		++StackOverflows;
		UE_LOG(LogMOBSimple, Log, TEXT("MOB_SIMPLE_STACK max=%d overflow=%d"), StackMax, StackOverflows);
	}
	FJumpLayer& Layer = JumpStack[JumpCount++];
	Layer.Clip = Clip;
	Layer.Time = 0.f;
	Layer.Fade = FadeSeconds > 0.f ? 0.f : 1.f;
	Layer.FadeSeconds = FadeSeconds;
	if (JumpCount > StackMax)
	{
		StackMax = JumpCount;
		UE_LOG(LogMOBSimple, Log, TEXT("MOB_SIMPLE_STACK max=%d overflow=%d"), StackMax, StackOverflows);
	}
}

// Distance 곡선에서 이 값이 되는 시각. 곡선은 시간이 갈수록 커진다(엔진 GetAnimPositionFromDistance 와 같은 가정)
float FMOBSimpleAnimProxy::TimeFromDistance(int32 Transition, float Value) const
{
	const TArray<float>& Table = TransitionDistances[Transition];
	const int32 Num = Table.Num();
	if (Num < 2) return 0.f;
	int32 First = 1;
	int32 Count = Num - 2;
	while (Count > 0)
	{
		const int32 Half = Count / 2;
		const int32 Middle = First + Half;
		if (Value > Table[Middle])
		{
			First = Middle + 1;
			Count -= Half + 1;
		}
		else
		{
			Count = Half;
		}
	}
	const float Span = Table[First] - Table[First - 1];
	const float Alpha = !FMath::IsNearlyZero(Span) ? FMath::Clamp((Value - Table[First - 1]) / Span, 0.f, 1.f) : 0.f;
	return FMath::Min(((float)(First - 1) + Alpha) / TRANSITION_TABLE_HZ, JumpLengths[kJump + Transition]);
}

float FMOBSimpleAnimProxy::DistanceAtTime(int32 Transition, float Time) const
{
	const TArray<float>& Table = TransitionDistances[Transition];
	if (Table.Num() == 0) return 0.f;
	const float Position = FMath::Clamp(Time * TRANSITION_TABLE_HZ, 0.f, (float)(Table.Num() - 1));
	const int32 Index = FMath::Min(FMath::FloorToInt32(Position), Table.Num() - 2);
	return Index < 0 ? Table[0] : FMath::Lerp(Table[Index], Table[Index + 1], Position - (float)Index);
}

// 전환 클립의 다음 시각 — 출발은 움직인 거리만큼, 멈춤은 남은 거리에 맞춰, 돌아서기는 방향이 바뀌기 전엔 남은 거리에 맞추고 바뀐 뒤엔 움직인 거리만큼 간다.
// 멈춤 곡선을 다 쓰면(속도 0) 그대로 시간을 흘린다
float FMOBSimpleAnimProxy::TransitionClipTime(const FJumpLayer& Layer, float DeltaSeconds) const
{
	const int32 Transition = Layer.Clip - kJump;
	const auto AdvanceByTravel = [&]()
	{
		const float Target = TimeFromDistance(Transition, DistanceAtTime(Transition, Layer.Time) + FrameTravel);
		const float Rate = DeltaSeconds > 0.f ? FMath::Clamp((Target - Layer.Time) / DeltaSeconds, DISTANCE_RATE_MIN, DISTANCE_RATE_MAX) : 1.f;
		return Layer.Time + Rate * DeltaSeconds;
	};
	switch (MoveState)
	{
	case EMoveState::Start:
		return AdvanceByTravel();
	case EMoveState::Stop:
		return StopDistance > 0.f ? FMath::Max(Layer.Time, TimeFromDistance(Transition, -StopDistance)) : Layer.Time + DeltaSeconds;
	case EMoveState::Pivot:
		return bBeforePivot ? FMath::Max(Layer.Time, TimeFromDistance(Transition, -PivotDistance)) : AdvanceByTravel();
	case EMoveState::Idle:
	case EMoveState::Cycle:
	case EMoveState::Air:
		break;
	}
	return Layer.Time + DeltaSeconds;
}

// 점프 쪽 층이 남아 있나 — 출발 · 멈춤 · 돌아서기 층은 세지 않는다(그 층이 빠지는 동안에도 대기 몸짓 시계는 흘러야 한다)
bool FMOBSimpleAnimProxy::HasJumpLayer() const
{
	for (int32 I = 0; I < JumpCount; ++I)
	{
		if (JumpStack[I].Clip != JUMP_NONE && JumpStack[I].Clip < kJump) return true;
	}
	return false;
}

const TCHAR* FMOBSimpleAnimProxy::MoveStateName(EMoveState State)
{
	switch (State)
	{
	case EMoveState::Idle: return TEXT("Idle");
	case EMoveState::Start: return TEXT("Start");
	case EMoveState::Cycle: return TEXT("Cycle");
	case EMoveState::Stop: return TEXT("Stop");
	case EMoveState::Pivot: return TEXT("Pivot");
	case EMoveState::Air: return TEXT("Air");
	}
	return TEXT("?");
}

void FMOBSimpleAnimProxy::EnterTransition(EMoveState State, int32 Direction, float FadeSeconds)
{
	const int32 Kind = State == EMoveState::Start ? TRANSITION_START : State == EMoveState::Stop ? TRANSITION_STOP : TRANSITION_PIVOT;
	MoveState = State;
	MoveDirection = Direction;
	FollowClip = INDEX_NONE;
	PushJump(kJump + Kind * 4 + Direction, FadeSeconds);
}

// 달리기 · 서기로 돌아간다
void FMOBSimpleAnimProxy::EnterLocomotion(EMoveState State, float FadeSeconds)
{
	FollowClip = INDEX_NONE;
	if (State == EMoveState::Cycle && JumpCount > 0 && JumpStack[JumpCount - 1].Clip >= kJump)
	{
		// 달리기 클립은 처음부터 시작해 같은 발 구간의 첫 자리로 간다. 확인: 왼발→오른발 구간이면 걸음 0, 오른발→왼발이면 클립 맨 끝 걸음(0초를 넘어 이어지는 구간) — 네 방향 다 StepCycle-1 이 그 구간이다
		const FJumpLayer& Top = JumpStack[JumpCount - 1];
		const int32 Transition = Top.Clip - kJump;
		const int32 Marker = StepMarkerAt(Transition, Top.Time);
		if (Marker != INDEX_NONE)
		{
			Step = TransitionMarkers[Transition][Marker - 1].bLeft ? 0 : StepCycle - 1;
			FollowClip = Top.Clip;
			FollowBaseStep = Step;
			FollowBaseMarker = Marker;
		}
	}
	if (State == EMoveState::Idle) IdleTime = 0.f;	// Lyra 는 서기 상태에 들어갈 때 서기 클립을 처음부터 튼다
	MoveState = State;
	PushJump(JUMP_NONE, FadeSeconds);
}

// 이 시각이 든 발 구간의 끝 표시 번호. 첫 표시 앞이나 마지막 표시 뒤면 INDEX_NONE
int32 FMOBSimpleAnimProxy::StepMarkerAt(int32 Transition, float Time) const
{
	const TArray<FStepMarker, TInlineAllocator<8>>& Markers = TransitionMarkers[Transition];
	for (int32 K = 1; K < Markers.Num(); ++K)
	{
		if (Time >= Markers[K - 1].Time && Time < Markers[K].Time) return K;
	}
	return INDEX_NONE;
}

// 나가는 전환 클립이 아직 더 무거우면 달리기 클립의 걸음을 그 클립의 발 표시에 맞춘다. 맞췄으면 true — 이 프레임엔 달리기 클립 자기 속도로 걷지 않는다
bool FMOBSimpleAnimProxy::FollowStepsOfTransition(float DeltaSeconds)
{
	if (FollowClip == INDEX_NONE) return false;
	float Layers[kJumpStack] = {};
	JumpWeights(Layers);
	for (int32 I = 0; I < JumpCount; ++I)
	{
		if (JumpStack[I].Clip != FollowClip) continue;
		if (Layers[I] < STEP_LEADER_WEIGHT) break;
		const int32 Transition = FollowClip - kJump;
		const float Time = JumpStack[I].Time + DeltaSeconds;	// 이 프레임에 그 클립이 평가될 시각 — 아래층은 UpdateJump 가 같은 만큼 민다
		const int32 Marker = StepMarkerAt(Transition, Time);
		if (Marker == INDEX_NONE) break;
		const TArray<FStepMarker, TInlineAllocator<8>>& Markers = TransitionMarkers[Transition];
		Step = (FollowBaseStep + Marker - FollowBaseMarker) % StepCycle;
		StepFrac = FMath::Clamp((Time - Markers[Marker - 1].Time) / FMath::Max(Markers[Marker].Time - Markers[Marker - 1].Time, 0.001f), 0.f, 1.f);
		return true;
	}
	FollowClip = INDEX_NONE;
	return false;
}

// 이동 상태 — Lyra 상태 기계의 Idle · Start · Cycle · Stop · Pivot 다섯을 실행값대로 흉내 낸다. 공중에서는 점프 쪽(UpdateJump)이 맡는다
void FMOBSimpleAnimProxy::UpdateTransitions(bool bFalling, bool bAccelerating, const FVector& LocalVelocity, const FVector& LocalAcceleration, const UCharacterMovementComponent* Move, float DeltaSeconds)
{
	if (bFalling)
	{
		MoveState = EMoveState::Air;
		return;
	}
	if (MoveState == EMoveState::Air) MoveState = bAccelerating ? EMoveState::Cycle : EMoveState::Idle;	// 땅에 닿을 때의 섞기는 UpdateJump 가 맡는다

	const float Speed = (float)LocalVelocity.Size2D();
	FrameTravel = Speed * DeltaSeconds;
	const FVector2D AccelDirection = FVector2D(LocalAcceleration).GetSafeNormal();
	bBeforePivot = bAccelerating && FVector2D::DotProduct(FVector2D(LocalVelocity), AccelDirection) < 0.f;
	StopDistance = PredictStopDistance(Speed, Move);
	PivotDistance = bBeforePivot && Move ? PredictPivotDistance(LocalAcceleration, LocalVelocity, Move->GroundFriction) : 0.f;

	const bool bPivotNeeded = bBeforePivot && Speed > PIVOT_MIN_SPEED;
	const float TopTime = JumpCount > 0 ? JumpStack[JumpCount - 1].Time : 0.f;
	const auto EnterPivot = [&]()
	{
		PivotAccelDirection = AccelDirection;
		EnterTransition(EMoveState::Pivot, CardinalFromAngle(AngleOf(LocalVelocity)), TO_PIVOT_SECONDS);
	};
	switch (MoveState)
	{
	case EMoveState::Idle:
		if (bAccelerating) EnterTransition(EMoveState::Start, CardinalFromAngle(AngleOf(LocalAcceleration)), TO_START_SECONDS);
		break;
	case EMoveState::Start:
		if (!bAccelerating && Speed < 1.f)	// 입력이 몸을 못 움직이고 끝났다(촬영 판 시작의 한 프레임 입력이 이 경우) — 멈출 것이 없어 서기로 바로 돌아간다. Lyra 는 여기서도 멈춤 상태를 거치지만 그 구간 기록이 없어 맞추지 않았다
		{
			MoveState = EMoveState::Idle;
			PushJump(JUMP_NONE, TO_START_SECONDS);
		}
		else if (!bAccelerating) EnterTransition(EMoveState::Stop, CardinalFromAngle(AngleOf(LocalVelocity)), TO_STOP_SECONDS);
		else if (bPivotNeeded) EnterPivot();
		else if (TopTime >= START_EXIT_TIMES[MoveDirection]) EnterLocomotion(EMoveState::Cycle, START_TO_CYCLE_SECONDS);
		break;
	case EMoveState::Cycle:
		if (!bAccelerating) EnterTransition(EMoveState::Stop, CardinalFromAngle(AngleOf(LocalVelocity)), TO_STOP_SECONDS);
		else if (bPivotNeeded) EnterPivot();
		break;
	case EMoveState::Stop:
		if (bAccelerating) EnterTransition(EMoveState::Start, CardinalFromAngle(AngleOf(LocalAcceleration)), STOP_TO_START_SECONDS);
		else if (Speed < 1.f && TopTime >= STOP_EXIT_TIMES[MoveDirection]) EnterLocomotion(EMoveState::Idle, STOP_TO_IDLE_SECONDS);
		break;
	case EMoveState::Pivot:
		if (!bAccelerating) EnterTransition(EMoveState::Stop, CardinalFromAngle(AngleOf(LocalVelocity)), TO_STOP_SECONDS);
		else if (bPivotNeeded && FVector2D::DotProduct(AccelDirection, PivotAccelDirection) < 0.f) EnterPivot();
		else if (TopTime >= PIVOT_EXIT_TIMES[MoveDirection]) EnterLocomotion(EMoveState::Cycle, PIVOT_TO_CYCLE_SECONDS);
		break;
	case EMoveState::Air:
		break;
	}
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
		if (bTransitions && I == JumpCount - 1 && Layer.Clip >= kJump) Layer.Time = TransitionClipTime(Layer, DeltaSeconds);
		else Layer.Time = Layer.Clip == JUMP_LAND && LandDistances.Num() > 0 ? FMath::Max(Layer.Time, LandTimeFromDistance(GroundDistance)) : Layer.Time + DeltaSeconds;
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

void FMOBSimpleAnimProxy::AppendPoseState(FString& Out) const
{
	Out += FString::Printf(TEXT(",%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d"), FidgetTime, RecoveryTime, AimBlend, Pitch, PlantOffset.X, PlantOffset.Y, PlantOffset.Z, JumpCount);
	for (int32 I = 0; I < kJumpStack; ++I) Out += FString::Printf(TEXT(",%d,%.9g,%.9g"), JumpStack[I].Clip, JumpStack[I].Time, JumpStack[I].Fade);
	Out += FString::Printf(TEXT(",%d,%d"), ResetGeneration, MoveSeq - 1);	// 이 줄의 상태 = 이 판의 입력을 이 번호까지 넣은 결과
}

void FMOBSimpleAnimProxy::AppendPlayers(FString& Out) const
{
	if (bTransitions) Out += FString::Printf(TEXT("~%s;"), MoveStateName(MoveState));	// 「@」 가 없는 칸 — 클립을 읽는 쪽은 건너뛴다. Lyra 상태 기계와 프레임을 맞춰 대조한다
	float Layers[kJumpStack] = {};
	JumpWeights(Layers);
	float Base = JumpCount == 0 ? 1.f : 0.f;
	for (int32 I = 0; I < JumpCount; ++I)
	{
		const FJumpLayer& Layer = JumpStack[I];
		if (Layer.Clip == JUMP_NONE || !JumpClips[Layer.Clip])
		{
			Base += Layers[I];
			continue;
		}
		if (Layers[I] >= 0.01f) Out += FString::Printf(TEXT("%s@%.3f*%.2f;"), *JumpClips[Layer.Clip]->GetName(), Layer.Time, Layers[I]);
	}
	for (int32 I = 0; I < kClips; ++I)
	{
		const float Weight = Weights[I] * Base;
		if (Clips[I] && Weight >= 0.01f) Out += FString::Printf(TEXT("%s@%.3f*%.2f;"), *Clips[I]->GetName(), Times[I], Weight);
	}
}

void FMOBSimpleAnimProxy::AppendClipDump(FString& Out, int32 SampleHz, const TSet<int32>& KeepBones, const TArray<const UAnimMontage*>& Montages, const UAnimSequence* OverridePose,
	const TArray<const UAnimSequence*>& AdsClips, const TArray<const UAnimSequence*>& CrouchClips) const
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

	// 클립 번호 = 걸음 5 · 조준 3 · 느슨한 조준 3 · 점프 5 · 전환 12 · 착지 충격 · 대기 몸짓(-MOBSimpleAimJump -MOBSimpleTransitions 일 때 30개, Shared/AnimClips.h 와 같은 차례).
	// 같은 판에서 그 뒤에 조준 클립 17개(30~46, 클립 파일 버전 3) · 앉기 클립 20개(47~66, 버전 4)가 붙는다.
	// 그 플래그가 없으면 예전처럼 앞의 5개뿐이다. 반복 여부는 Evaluate 와 같게 준다 — 표본을 끝까지 읽을 때 앞으로 감기는지가 달라진다
	struct FDumpClip { const UAnimSequence* Clip; float Length; bool bLoop; float RootSpeed; };
	TArray<FDumpClip, TInlineAllocator<32>> All;
	for (int32 I = 0; I < kClips; ++I) All.Add({ Clips[I], Lengths[I], true, RootSpeeds[I] });
	if (bAimJump && bTransitions)
	{
		const auto Add = [&All](const UAnimSequence* Clip, bool bLoop) { All.Add({ Clip, Clip ? FMath::Max(Clip->GetPlayLength(), 0.01f) : 0.f, bLoop, 0.f }); };
		for (int32 I = 0; I < kAim; ++I) Add(AimClips[I], false);
		for (int32 I = 0; I < kAim; ++I) Add(ReadyAimClips[I], false);
		for (int32 I = 0; I < kStackClips; ++I) Add(JumpClips[I], I == JUMP_START_LOOP || I == JUMP_FALL_LOOP);
		Add(RecoveryClip, false);
		Add(FidgetClip, false);
		// 조준 클립 17개(클립 파일 버전 3, D60) — 서기 · 걷기 넷은 반복 · 원래 속도를 BindSet 과 같은 식으로, 전환 12개는 한 번 재생
		for (int32 I = 0; I < kAdsClips; ++I)
		{
			const UAnimSequence* Clip = AdsClips.IsValidIndex(I) ? AdsClips[I] : nullptr;
			const bool bBase = I < kClips;
			const float Length = Clip ? FMath::Max(Clip->GetPlayLength(), 0.01f) : 0.f;
			const float RootSpeed = Clip && bBase ? (float)Clip->ExtractRootMotionFromRange(0.0, Length, FAnimExtractContext()).GetTranslation().Size() / Length : 0.f;
			All.Add({ Clip, Length, bBase, RootSpeed });
		}
		// 앉기 클립 20개(버전 4, D-3) — 조준 클립과 같은 식. 서기 · 걷기 넷만 반복하고, 전환 12 · 앉기/일어서기 전환 · 덮기 자세는 한 번 재생
		for (int32 I = 0; I < kCrouchClips; ++I)
		{
			const UAnimSequence* Clip = CrouchClips.IsValidIndex(I) ? CrouchClips[I] : nullptr;
			const bool bBase = I < kClips;
			const float Length = Clip ? FMath::Max(Clip->GetPlayLength(), 0.01f) : 0.f;
			const float RootSpeed = Clip && bBase ? (float)Clip->ExtractRootMotionFromRange(0.0, Length, FAnimExtractContext()).GetTranslation().Size() / Length : 0.f;
			All.Add({ Clip, Length, bBase, RootSpeed });
		}
		Out += FString::Printf(TEXT("AIMPITCH,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n"), AimPitches[AIM_UP], AimPitches[AIM_CENTER], AimPitches[AIM_DOWN], ReadyAimPitches[AIM_UP], ReadyAimPitches[AIM_CENTER], ReadyAimPitches[AIM_DOWN]);
	}
	const int32 BaseClipCount = All.Num();

	// 몽타주(9단계 3-a) — 줄(끼움 자리)마다 든 클립을 30개 뒤에 붙인다. 이미 있는 클립(같은 자산)이면 그 번호를 쓴다 — 빼기 몽타주는 권총 꺼내기 클립의 한 구간이고,
	// 소총 덮기 자세는 서기 클립 그 자체다. 몽타주 클립은 몽타주 안에서 한 번만 돌므로(기록 x1) 반복하지 않고 읽는다
	const auto ClipIndex = [&All](const UAnimSequence* Clip) -> int32
	{
		if (!Clip) return INDEX_NONE;
		for (int32 I = 0; I < All.Num(); ++I)
		{
			if (All[I].Clip == Clip) return I;
		}
		All.Add({ Clip, FMath::Max(Clip->GetPlayLength(), 0.01f), false, 0.f });
		return All.Num() - 1;
	};
	FString MontageLines;
	const bool bMontages = bAimJump && bTransitions;
	for (int32 M = 0; bMontages && M < Montages.Num(); ++M)
	{
		const UAnimMontage* Montage = Montages[M];
		if (!Montage)
		{
			MontageLines += FString::Printf(TEXT("MONTAGEFAIL,%d\n"), M);
			continue;
		}
		FString Sections;
		for (const FCompositeSection& S : Montage->CompositeSections) Sections += FString::Printf(TEXT("%s@%.4f>%s;"), *S.SectionName.ToString(), S.GetTime(), *S.NextSectionName.ToString());
		// 칸 차례 = 1-a 기록 도구의 몽타주 기록(animcmp_ev_<무기>_montages.csv)과 같다 — 3-a 대조가 칸끼리 맞춘다
		MontageLines += FString::Printf(TEXT("MONTAGE,%d,%s,%s,%.9g,%.9g,%.9g,%d,%.9g,%d,%.9g,%d,%d,%d,%s,%s,%s\n"), M, *Montage->GetName(), *Montage->GetGroupName().ToString(),
			Montage->GetPlayLength(), Montage->RateScale, Montage->BlendIn.GetBlendTime(), (int32)Montage->BlendIn.GetBlendOption(), Montage->BlendOut.GetBlendTime(), (int32)Montage->BlendOut.GetBlendOption(),
			Montage->BlendOutTriggerTime, Montage->bEnableAutoBlendOut ? 1 : 0, (int32)Montage->BlendModeIn, (int32)Montage->BlendModeOut,
			*GetNameSafe(Montage->BlendProfileIn.Get()), *GetNameSafe(Montage->BlendProfileOut.Get()), *Sections.Replace(TEXT(","), TEXT(" ")));
		// 몽타주 자체 곡선 — 몽타주 시각 기준(꺼내기의 DisableLHandIK 등, 1-c)
		for (const FFloatCurve& Curve : Montage->GetCurveData().FloatCurves)
		{
			const int32 N = FMath::CeilToInt32(Montage->GetPlayLength() * MONTAGE_CURVE_HZ) + 1;
			MontageLines += FString::Printf(TEXT("MCURVE,%d,%s,%d,%d"), M, *Curve.GetName().ToString(), MONTAGE_CURVE_HZ, N);
			for (int32 S = 0; S < N; ++S) MontageLines += FString::Printf(TEXT(",%.6f"), Curve.Evaluate(FMath::Min((float)S / MONTAGE_CURVE_HZ, Montage->GetPlayLength())));
			MontageLines += TEXT("\n");
		}
		for (const FSlotAnimationTrack& Track : Montage->SlotAnimTracks)
		{
			for (const FAnimSegment& Segment : Track.AnimTrack.AnimSegments)
			{
				const UAnimSequence* Clip = Cast<UAnimSequence>(Segment.GetAnimReference().Get());
				const int32 Index = ClipIndex(Clip);
				if (Index == INDEX_NONE)
				{
					MontageLines += FString::Printf(TEXT("TRACKFAIL,%d,%s,%s\n"), M, *Track.SlotName.ToString(), *GetNameSafe(Segment.GetAnimReference().Get()));
					continue;
				}
				MontageLines += FString::Printf(TEXT("TRACK,%d,%s,%d,%.9g,%.9g,%.9g,%.9g,%d\n"), M, *Track.SlotName.ToString(), Index, Segment.StartPos, Segment.AnimStartTime, Segment.AnimEndTime, Segment.AnimPlayRate, Segment.LoopingCount);
			}
		}
	}
	if (bMontages)
	{
		const int32 Override = ClipIndex(OverridePose);
		if (Override == INDEX_NONE) MontageLines += TEXT("OVERRIDEFAIL\n");
		else MontageLines += FString::Printf(TEXT("OVERRIDE,%d\n"), Override);
	}

	for (int32 I = 0; I < All.Num(); ++I)
	{
		const FDumpClip& Entry = All[I];
		if (!Entry.Clip)
		{
			Out += FString::Printf(TEXT("CLIPFAIL,%d\n"), I);
			continue;
		}
		const int32 Samples = FMath::CeilToInt32(Entry.Length * SampleHz) + 1;
		Out += FString::Printf(TEXT("CLIP,%d,%s,%.9g,%d,%d,%.6f,%d\n"), I, *Entry.Clip->GetName(), Entry.Length, Samples, (int32)Entry.Clip->GetAdditiveAnimType(), Entry.RootSpeed, Entry.bLoop ? 1 : 0);
		for (const FAnimSyncMarker& Marker : Entry.Clip->AuthoredSyncMarkers) Out += FString::Printf(TEXT("MARK,%d,%.6f,%s\n"), I, Marker.Time, *Marker.MarkerName.ToString());
		for (int32 S = 0; S < Samples; ++S)
		{
			const double Time = FMath::Min((double)S / SampleHz, (double)Entry.Length);
			FCompactPose Pose;
			Pose.SetBoneContainer(&Bones);
			FBlendedCurve Curve;
			Curve.InitFrom(Bones);
			UE::Anim::FStackAttributeContainer Attributes;
			FAnimationPoseData Data(Pose, Curve, Attributes);
			Entry.Clip->GetAnimationPose(Data, FAnimExtractContext(Time, false, FDeltaTimeRecord(), Entry.bLoop));
			for (int32 B = 0; B < NumBones; ++B)
			{
				if (!KeepBones.Contains(B)) continue;
				Line(*FString::Printf(TEXT("KEY,%d,%d,%d"), I, S, B), Pose[FCompactPoseBoneIndex(B)]);
			}
		}
	}

	// 거리 곡선 표 — PreUpdate 에서 뽑아 둔 값 그대로. 전환 클립 = 움직인 거리, 착지 클립 = 땅까지 거리(양수로 뒤집은 값)
	const auto CurveLine = [&Out](int32 Clip, const TCHAR* Name, int32 Hz, const TArray<float>& Values)
	{
		Out += FString::Printf(TEXT("CURVE,%d,%s,%d,%d"), Clip, Name, Hz, Values.Num());
		for (const float V : Values) Out += FString::Printf(TEXT(",%.6f"), V);
		Out += TEXT("\n");
	};
	if (bAimJump && bTransitions)
	{
		constexpr int32 FIRST_JUMP_INDEX = kClips + kAim + kAim;
		for (int32 T = 0; T < kTransitions; ++T) CurveLine(FIRST_JUMP_INDEX + kJump + T, TEXT("Distance"), TRANSITION_TABLE_HZ, TransitionDistances[T]);
		CurveLine(FIRST_JUMP_INDEX + JUMP_LAND, TEXT("GroundDistance"), LAND_TABLE_HZ, LandDistances);
		// 조준 전환 12개의 거리 곡선 — 허리 전환 표(BindSet)와 같은 식으로 뽑는다
		constexpr int32 FIRST_ADS_INDEX = FIRST_JUMP_INDEX + kStackClips + 2;	// 착지 충격 · 대기 몸짓 뒤 = 30
		for (int32 T = 0; T < kTransitions; ++T)
		{
			const UAnimSequence* Clip = AdsClips.IsValidIndex(kClips + T) ? AdsClips[kClips + T] : nullptr;
			if (!Clip) continue;
			const float Length = FMath::Max(Clip->GetPlayLength(), 0.01f);
			const int32 Samples = FMath::CeilToInt32(Length * TRANSITION_TABLE_HZ) + 1;
			TArray<float> Values;
			for (int32 S = 0; S < Samples; ++S) Values.Add(Clip->EvaluateCurveData(TRANSITION_DISTANCE_CURVE, FAnimExtractContext(FMath::Min((double)S / TRANSITION_TABLE_HZ, (double)Length))));
			CurveLine(FIRST_ADS_INDEX + kClips + T, TEXT("Distance"), TRANSITION_TABLE_HZ, Values);
		}
		// 앉은 전환 12개의 거리 곡선(D-3) — 조준 전환과 같은 식. 앉기 묶음은 조준 클립 17개 바로 뒤(47)부터
		constexpr int32 FIRST_CROUCH_INDEX = FIRST_ADS_INDEX + kAdsClips;
		for (int32 T = 0; T < kTransitions; ++T)
		{
			const UAnimSequence* Clip = CrouchClips.IsValidIndex(kClips + T) ? CrouchClips[kClips + T] : nullptr;
			if (!Clip) continue;
			const float Length = FMath::Max(Clip->GetPlayLength(), 0.01f);
			const int32 Samples = FMath::CeilToInt32(Length * TRANSITION_TABLE_HZ) + 1;
			TArray<float> Values;
			for (int32 S = 0; S < Samples; ++S) Values.Add(Clip->EvaluateCurveData(TRANSITION_DISTANCE_CURVE, FAnimExtractContext(FMath::Min((double)S / TRANSITION_TABLE_HZ, (double)Length))));
			CurveLine(FIRST_CROUCH_INDEX + kClips + T, TEXT("Distance"), TRANSITION_TABLE_HZ, Values);
		}
	}

	// 몽타주 표 줄과, 67개(조준 클립 17개 · 앉기 클립 20개 포함) 뒤에 붙인 클립의 곡선 전부(클립 시각 기준 — 소총 재장전의 DisableLHandIK 는 몽타주가 아니라 클립에 있다, 1-c)
	Out += MontageLines;
	for (int32 I = BaseClipCount; I < All.Num(); ++I)
	{
		const UAnimSequence* Clip = All[I].Clip;
		for (const FFloatCurve& Curve : Clip->GetCurveData().FloatCurves)
		{
			TArray<float> Values;
			const int32 N = FMath::CeilToInt32(All[I].Length * MONTAGE_CURVE_HZ) + 1;
			for (int32 S = 0; S < N; ++S) Values.Add(Clip->EvaluateCurveData(Curve.GetName(), FAnimExtractContext(FMath::Min((double)S / MONTAGE_CURVE_HZ, (double)All[I].Length))));
			Out += FString::Printf(TEXT("SCURVE,%d,%s,%d,%d"), I, *Curve.GetName().ToString(), MONTAGE_CURVE_HZ, Values.Num());
			for (const float V : Values) Out += FString::Printf(TEXT(",%.6f"), V);
			Out += TEXT("\n");
		}
	}
}

// 읽은 한 벌을 재생 멤버에 묶는다. 서버 구동이면 무기마다 한 번씩 불러 사본을 뜬다(로그도 한 벌마다 한 번)
void FMOBSimpleAnimProxy::BindSet(const UMOBSimpleAnimInstance& Instance, int32 SetIndex)
{
	const FMOBLoadedClipSet& Set = Instance.LoadedSets[SetIndex];
	for (int32 I = 0; I < kClips; ++I)
	{
		const UAnimSequence* Clip = Set.Clips.IsValidIndex(I) ? Set.Clips[I].Get() : nullptr;
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
		UE_LOG(LogMOBSimple, Log, TEXT("MOB_SIMPLE_CLIP name=%s length=%.3f rootSpeed=%.1f steps=%d markers=[%s]"), *GetNameSafe(Clip), Lengths[I], RootSpeeds[I], StepTimes[I].Num(), *MarkerList);
		if (I != CLIP_IDLE && (StepTimes[I].Num() == 0 || StepTimes[I].Num() % 2 != 0))
		{
			UE_LOG(LogMOBSimple, Error, TEXT("MOB_SIMPLE_FAIL steps name=%s count=%d"), *GetNameSafe(Clip), StepTimes[I].Num());
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
		AimClips[I] = Set.Aim.IsValidIndex(I) ? Set.Aim[I].Get() : nullptr;
		AimPitches[I] = Set.AimPitches[I];
		ReadyAimClips[I] = Instance.LoadedReadyAim.IsValidIndex(I) ? Instance.LoadedReadyAim[I].Get() : nullptr;
		ReadyAimPitches[I] = Instance.ReadyAimPitches[I];
	}
	AimAdditiveType = Additive(AimClips[AIM_CENTER]);
	ReadyAimAdditiveType = Additive(ReadyAimClips[AIM_CENTER]);
	for (int32 I = 0; I < kStackClips && bAimJump; ++I)
	{
		JumpClips[I] = Set.Jump.IsValidIndex(I) ? Set.Jump[I].Get() : nullptr;
		JumpLengths[I] = JumpClips[I] ? FMath::Max(JumpClips[I]->GetPlayLength(), 0.01f) : 0.f;
	}
	// 전환 클립의 거리 곡선과 발 표시를 뽑아 둔다. 표시 이름까지 로그에 남겨 넘어가는 때의 근거를 대조한다
	for (int32 T = 0; T < kTransitions && bTransitions; ++T)
	{
		const UAnimSequence* Clip = JumpClips[kJump + T];
		TransitionDistances[T].Reset();
		TransitionMarkers[T].Reset();
		if (!Clip) continue;
		const float Length = JumpLengths[kJump + T];
		const int32 Samples = FMath::CeilToInt32(Length * TRANSITION_TABLE_HZ) + 1;
		for (int32 S = 0; S < Samples; ++S)
		{
			TransitionDistances[T].Add(Clip->EvaluateCurveData(TRANSITION_DISTANCE_CURVE, FAnimExtractContext(FMath::Min((double)S / TRANSITION_TABLE_HZ, (double)Length))));
		}
		FString Steps;
		for (const FAnimSyncMarker& Marker : Clip->AuthoredSyncMarkers)
		{
			FStepMarker& Added = TransitionMarkers[T].AddDefaulted_GetRef();
			Added.Time = Marker.Time;
			Added.bLeft = Marker.MarkerName == TEXT("L");
			Steps += FString::Printf(TEXT("%s@%.3f "), *Marker.MarkerName.ToString(), Marker.Time);
		}
		FString Notifies;
		for (const FAnimNotifyEvent& Notify : Clip->Notifies)
		{
			if (Notify.NotifyStateClass) Notifies += FString::Printf(TEXT("%s@%.3f+%.3f "), *Notify.NotifyStateClass->GetClass()->GetName(), Notify.GetTriggerTime(), Notify.GetDuration());
		}
		int32 Rises = 0;	// 시간이 가며 거리가 다시 작아지는 칸 수 — 0 이어야 거리로 시각을 하나로 찾는다
		for (int32 K = 1; K < TransitionDistances[T].Num(); ++K) Rises += TransitionDistances[T][K] < TransitionDistances[T][K - 1] - 0.01f ? 1 : 0;
		UE_LOG(LogMOBSimple, Log, TEXT("MOB_SIMPLE_TCLIP name=%s length=%.3f dist=%.1f..%.1f falls=%d markers=[%s] states=[%s]"), *Clip->GetName(), Length, TransitionDistances[T][0], TransitionDistances[T].Last(), Rises, *Steps, *Notifies);
	}
	RecoveryClip = bAimJump ? Set.Recovery.Get() : nullptr;
	RecoveryLength = RecoveryClip ? FMath::Max(RecoveryClip->GetPlayLength(), 0.01f) : 0.f;
	RecoveryAdditiveType = Additive(RecoveryClip);
	FidgetClip = bAimJump ? Set.Fidget.Get() : nullptr;
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

void FMOBSimpleAnimProxy::PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds)
{
	FAnimInstanceProxy::PreUpdate(InAnimInstance, DeltaSeconds);

	UMOBSimpleAnimInstance* Instance = CastChecked<UMOBSimpleAnimInstance>(InAnimInstance);
	if (!Instance->bClipsLoaded)
	{
		Instance->LoadClips();
		bAimJump = Instance->bAimJump;
		bTransitions = Instance->bTransitions;

		// 한 벌씩 묶어 사본을 떠 두고 처음 쓸 한 벌을 넣는다 — 서버 무기를 이미 받았으면 그 한 벌, 아니면 명령줄 무기
		ClipSets.SetNum(Instance->LoadedSets.Num());
		for (int32 S = 0; S < ClipSets.Num(); ++S)
		{
			if (!Instance->LoadedSets[S].Key) continue;
			BindSet(*Instance, S);
			SaveSet(ClipSets[S]);
			const TArray<TObjectPtr<UAnimMontage>>& Montages = Instance->LoadedSets[S].Montages;
			for (int32 K = 0; K < kMontageKindCount; ++K) ClipSets[S].Montages[K] = Montages.IsValidIndex(K) ? Montages[K].Get() : nullptr;
		}
		ActiveSet = ClipSets.IsValidIndex(WantedSet) && ClipSets[WantedSet].bLoaded ? WantedSet : Instance->PrimarySet;
		if (ClipSets.IsValidIndex(ActiveSet) && ClipSets[ActiveSet].bLoaded) UseSet(ClipSets[ActiveSet]);
	}

	if (bServerDriven) return;	// 재생 상태는 SetServerFrame 이 넣는다 — 여기서 시간을 쌓지 않는다

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
	const bool bStepsFollow = FollowStepsOfTransition(DeltaSeconds);
	if (!bStepsFollow && DirectionWeight > 0.001f && StepSeconds > UE_SMALL_NUMBER)
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
		if (!bTransitions && !bFalling && bWasAccelerating && !bAccelerating && Lengths[CLIP_IDLE] > STOP_CLIP_SECONDS) IdleTime = Lengths[CLIP_IDLE] - STOP_CLIP_SECONDS;
		bWasAccelerating = bAccelerating;

		// 발 디딤 내림 — 땅에선 잰 거리만큼, 뜬 직후엔 마지막으로 잰 거리만큼 내린다. bWasFalling 은 아직 지난 프레임 값이다
		bool bGapMeasured = false;
		float MeasuredGap = 0.f;
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
				bGapMeasured = true;
				MeasuredGap = Gap;
			}
		}
		if (bFalling) bPlantLogged = false;
		const bool bPlant = bFalling ? AirTime < PLANT_RELEASE_SECONDS - PLANT_TIME_SLACK : GroundTime >= PLANT_LAND_DELAY - PLANT_TIME_SLACK;
		const USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
		PlantOffset = bPlant && Mesh ? Mesh->GetComponentTransform().InverseTransformVectorNoScale(FVector(0.0, 0.0, -PlantDrop)) : FVector::ZeroVector;

		// 대조 기록용 — 언리얼 밖 상태 계산의 입력 원본. 판 시작부터 매 프레임 남긴다(기록하지 않는 대기 프레임에도 상태는 움직인다)
		const FVector LocalAccelLog = Owner && Move ? Owner->GetActorRotation().UnrotateVector(Move->GetCurrentAcceleration()) : FVector::ZeroVector;
		MoveLog += FString::Printf(TEXT("%d,%d,%.9g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%d,%d,%.9g,%.9g,%d,%.9g\n"), ResetGeneration, MoveSeq, DeltaSeconds, Local.X, Local.Y, Local.Z, LocalAccelLog.X, LocalAccelLog.Y, LocalAccelLog.Z,
			bAccelerating ? 1 : 0, bFalling ? 1 : 0, Pitch, GroundDistance, bGapMeasured ? 1 : 0, MeasuredGap);
		++MoveSeq;

		if (bTransitions && Owner && Move)
		{
			const EMoveState Before = MoveState;
			UpdateTransitions(bFalling, bAccelerating, Local, Owner->GetActorRotation().UnrotateVector(Move->GetCurrentAcceleration()), Move, DeltaSeconds);
			if (MoveState != Before) UE_LOG(LogMOBSimple, Log, TEXT("MOB_SIMPLE_STATE %s>%s dir=%d speed=%.1f accel=%d"), MoveStateName(Before), MoveStateName(MoveState), MoveDirection, (float)Local.Size2D(), bAccelerating ? 1 : 0);
		}
		UpdateJump(bFalling, bAccelerating, (float)Velocity.Z, TimeToApex, GroundDistance, DeltaSeconds);
		UpdateIdleBreak(!bFalling && !bAccelerating && Speed < 1.f && (bTransitions ? !HasJumpLayer() : JumpCount == 0), DeltaSeconds);
	}
}

// 받은 몽타주 칸을 끼움 자리 다섯에 얹는다(9단계 5-a · 다섯째 맞은 쪽 반응은 컨텐츠 E-2, 손가락 · 꼬임 뼈 몫 — 판정 뼈는 끝에서 서버 자세가 덮는다).
// 칸은 엔진 몽타주 평가 자료(MontageEvaluationData)로 넣는다. 엔진은 갱신마다 재생 중인 몽타주로 이 자료를 다시 채우는데(UAnimInstance::UpdateMontageEvaluationData)
// 이 인스턴스는 몽타주를 틀지 않아 빈 채로 온다 → 평가 안에서 채워 이번 평가가 쓰고, 다음 갱신이 다시 비운다.
// 자리 차례 · 뼈 가림은 서버 자세 계산과 같다(Shared/AnimPose.cpp EvaluateLocal · ApplySlot, D72 — Lyra ABP_Mannequin_Base 그래프 차례). 처음엔 자리마다 SlotEvaluatePose 뒤, 비중 표가 있으면 아래 자세와 뼈별로 섞었다 · 메시 기준 회전)
void FMOBSimpleAnimProxy::ApplyMontages(FPoseContext& Output, bool bAfterAim)
{
	static_assert(kMontageCells == mo::kAnimMontageMax, "몽타주 칸 수 = Shared kAnimMontageMax");
	static_assert(kMontageKindCount == mo::MONTAGE_KIND_COUNT && kMontageKinds == mo::MONTAGE_KIND_COUNT, "몽타주 종류 수 = Shared MONTAGE_KIND_COUNT");
	// 윗몸 자리 비중 표 = Lyra 그래프 LayeredBoneBlend_2 의 가림(make_clip_pack.py SLOTS 윗몸 자리와 같다, D72). 처음엔 UpperBodyMask(D50)
	static const FName UPPER_BODY_MASK(TEXT("UpperBodyLowerBodySplitMask"));
	static const FName FULL_BODY(TEXT("FullBody"));
	static const FName PRE_AIM(TEXT("FullBodyAdditivePreAim"));
	static const FName UPPER_BODY(TEXT("UpperBody"));
	static const FName UPPER_BODY_ADDITIVE(TEXT("UpperBodyAdditive"));
	static const FName HIT_REACT(TEXT("AdditiveHitReact"));

	// 가림 없는 자리 — 엔진 끼움 노드(FAnimNode_Slot::Evaluate_AnyThread)처럼 아래 자세 위에 바로 얹는다
	const auto ApplyPlainSlot = [this, &Output](const FName& Name)
	{
		float SlotWeight = 0.f, SourceWeight = 0.f, TotalWeight = 0.f;
		GetSlotWeight(Name, SlotWeight, SourceWeight, TotalWeight);
		if (SlotWeight <= ZERO_ANIMWEIGHT_THRESH) return;
		++SlotPasses;
		FPoseContext Source(Output);
		Source = Output;
		FAnimationPoseData SourceData(Source);
		FAnimationPoseData OutputData(Output);
		SlotEvaluatePose(Name, SourceData, SourceWeight, OutputData, SlotWeight, TotalWeight);
	};

	if (bAfterAim)
	{
		// 조준 층 뒤 — 맞은 쪽 반응 → (덧붙임 층 FullBodyAdditives, 안 옮김) → 통째. 평가 자료는 조준 앞 단계가 이번 평가에 채워 둔 것이다
		if (GetMontageEvaluationData().Num() == 0) return;
		ApplyPlainSlot(HIT_REACT);
		ApplyPlainSlot(FULL_BODY);
		return;
	}

	SlotPasses = 0;
	TArray<FMontageEvaluationState>& Data = GetMontageEvaluationData();
	Data.Reset();
	for (int32 I = 0; I < MontageCellCount; ++I)
	{
		const FMontageCell& Cell = MontageCells[I];
		// 엔진이 쓰는 비중 = BlendInfo.GetBlendedValue(). 시작 0 · 목표 = 받은 비중 · 진행 1 이면 꼭 받은 비중이다(FAlphaBlend::SetAlpha, 기본 직선)
		FAlphaBlend Blend;
		Blend.SetValueRange(0.f, Cell.Weight);
		Blend.SetAlpha(1.f);
		Data.Emplace(Cell.Montage, Cell.Position, FDeltaTimeRecord(), true, true, Blend, nullptr, 1.f);
	}
	if (Data.Num() == 0) return;

	// 윗몸 뼈 가림 — 엔진 FAnimNode_LayeredBoneBlend::UpdateCachedBoneData 와 같은 차례로 만든다(뼈대 차례 → 압축 뼈 차례, 층 비중 1)
	const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
	if (UpperMaskSerial != Bones.GetSerialNumber() || UpperMaskWeights.Num() != Output.Pose.GetNumBones())
	{
		UpperMaskSerial = Bones.GetSerialNumber();
		UpperMaskWeights.Reset();
		USkeleton* MaskSkeleton = GetSkeleton();
		UBlendProfile* Mask = MaskSkeleton ? MaskSkeleton->GetBlendProfile(UPPER_BODY_MASK) : nullptr;
		bUpperMaskMissing = !Mask || Mask->Mode != EBlendProfileMode::BlendMask;
		if (!bUpperMaskMissing)
		{
			TArray<FPerBoneBlendWeight> BySkeleton;
			const TArray<UBlendProfile*> Masks = { Mask };
			FAnimationRuntime::CreateMaskWeights(BySkeleton, Masks, MaskSkeleton);
			const TArray<FBoneIndexType>& Required = Bones.GetBoneIndicesArray();
			TArray<FPerBoneBlendWeight> Desired;
			Desired.SetNumZeroed(Required.Num());
			for (int32 R = 0; R < Required.Num(); ++R)
			{
				const int32 SkeletonBone = Bones.GetSkeletonIndex(FCompactPoseBoneIndex(R));
				if (BySkeleton.IsValidIndex(SkeletonBone)) Desired[R] = BySkeleton[SkeletonBone];
			}
			UpperMaskWeights.SetNumZeroed(Required.Num());
			float LayerWeights[1] = { 1.f };
			FAnimationRuntime::UpdateDesiredBoneWeight(Desired, UpperMaskWeights, TArrayView<float>(LayerWeights, 1));
		}
	}

	// 조준 층 앞 — LayeredBoneBlend_2: 바탕 = 아래 자세 + UpperBodyAdditive(ApplyAdditive_1, 가림 없음) · 섞을 쪽 = 아래 자세 위 UpperBody, 가림 = 윗몸 자리 비중 표.
	// 그다음 사격 덧붙임(FullBodyAdditivePreAim). ApplyAdditive_1 의 비중(UpperbodyDynamicAdditiveWeight)은 서버처럼 늘 1 이다(D72 남은 차이)
	float UpperWeight = 0.f, UpperSource = 0.f, UpperTotal = 0.f;
	float AdditiveWeight = 0.f, AdditiveSource = 0.f, AdditiveTotal = 0.f;
	GetSlotWeight(UPPER_BODY, UpperWeight, UpperSource, UpperTotal);
	GetSlotWeight(UPPER_BODY_ADDITIVE, AdditiveWeight, AdditiveSource, AdditiveTotal);
	const bool bUpper = UpperWeight > ZERO_ANIMWEIGHT_THRESH;
	const bool bAdditive = AdditiveWeight > ZERO_ANIMWEIGHT_THRESH;
	if (bUpper || bAdditive)
	{
		FPoseContext Locomotion(Output);
		Locomotion = Output;
		FPoseContext Upper(Output);
		if (bUpper)
		{
			++SlotPasses;
			FAnimationPoseData SourceData(Locomotion);
			FAnimationPoseData UpperData(Upper);
			SlotEvaluatePose(UPPER_BODY, SourceData, UpperSource, UpperData, UpperWeight, UpperTotal);
		}
		else
		{
			Upper = Locomotion;
		}
		FPoseContext Base(Output);
		if (bAdditive)
		{
			++SlotPasses;
			FAnimationPoseData SourceData(Locomotion);
			FAnimationPoseData BaseData(Base);
			SlotEvaluatePose(UPPER_BODY_ADDITIVE, SourceData, AdditiveSource, BaseData, AdditiveWeight, AdditiveTotal);
		}
		else
		{
			Base = Locomotion;
		}
		if (bUpperMaskMissing)
		{
			// 표를 못 찾음 — 윗몸 자리를 통째로(있으면), 없으면 덧붙인 바탕
			Output = bUpper ? Upper : Base;
		}
		else
		{
			TArray<FCompactPose, TInlineAllocator<1>> Targets;
			TArray<FBlendedCurve, TInlineAllocator<1>> TargetCurves;
			TArray<UE::Anim::FStackAttributeContainer, TInlineAllocator<1>> TargetAttributes;
			Targets.SetNum(1);
			TargetCurves.SetNum(1);
			TargetAttributes.SetNum(1);
			Targets[0].MoveBonesFrom(Upper.Pose);
			TargetCurves[0].MoveFrom(Upper.Curve);
			TargetAttributes[0].MoveFrom(Upper.CustomAttributes);
			FAnimationPoseData OutputData(Output);
			FAnimationRuntime::BlendPosesPerBoneFilter(Base.Pose, Targets, Base.Curve, TargetCurves, Base.CustomAttributes, TargetAttributes, OutputData, UpperMaskWeights,
				FAnimationRuntime::EBlendPosesPerBoneFilterFlags::MeshSpaceRotation, ECurveBlendOption::Override);
		}
	}
	ApplyPlainSlot(PRE_AIM);
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
		if (bServerDriven) ApplyServerBones(Output);
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
		// 서버 구동이면 착지 충격과 조준 사이에 몽타주 자리 넷을 얹는다 — 서버 자세 계산과 같은 차례(Shared/AnimPose.cpp EvaluateLocal, D50)
		// 컨텐츠 E-2-b 뒤로는 Lyra 그래프 차례(D72) — 조준 앞에 윗몸 섞기 · 사격 덧붙임, 조준 뒤에 맞은 쪽 반응 · 통째
		ApplyJump(Output);
		ApplyRecovery(Output);
		if (bServerDriven) ApplyMontages(Output, false);
		ApplyAim(Output);
		if (bServerDriven) ApplyMontages(Output, true);
		ApplyHandIK(Output);	// 실행값: 손은 IK 총 뼈에 붙고(공중 거리 0.0), 그 뒤 발 디딤 내림이 손까지 함께 내린다(땅 거리 4.0)
	}

	// 위치는 서버 값이다 — 클립의 뿌리 이동이 몸을 끌고 가지 않게 뿌리를 제자리에 묶는다
	const FCompactPoseBoneIndex RootBone(0);
	Output.Pose[RootBone] = Output.Pose.GetRefPose(RootBone);
	if (bAimJump) ApplyPlant(Output);
	if (bServerDriven) ApplyServerBones(Output);
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

void UMOBSimpleAnimInstance::SetServerFrame(const mo::AnimPoseInput& Input, int32 Weapon, const int32* MontageWeapons, const mo::BoneTransform* Local, int32 Count, const TArray<FName>& BoneNames, FMOBServerAnimResult& Out)
{
	GetProxyOnGameThread<FMOBSimpleAnimProxy>().SetServerFrame(Input, Weapon, MontageWeapons, Local, Count, BoneNames, Out);
}

void UMOBSimpleAnimInstance::StepServerMontages(const mo::AnimPoseInput& Input, uint32 StateTick, const int32* MontageWeapons, bool bContinue, FMOBServerAnimResult& Out)
{
	TArray<FMOBSimpleAnimProxy::FMontageStep, TInlineAllocator<FMOBSimpleAnimProxy::kMontageCells>> Steps;
	GetProxyOnGameThread<FMOBSimpleAnimProxy>().StepMontages(Input, StateTick, MontageWeapons, bContinue, Out, Steps);
	++Out.StepStates;

	// 몽타주 알림 — 칸마다 앞 상태에서 지난 구간(앞 시각 < 알림 시각 ≤ 지금 시각, 새 칸은 0 부터)에 든 것만 튼다.
	// 트는 것 = 무기 메시 몽타주(Lyra 기록의 AN_PlayWeaponMontage — 총의 노리쇠 · 탄창 움직임) · 소리(AnimNotify_PlaySound — 재장전 · 꺼내기 소리).
	// 나머지(SaveAttack · ResetCombo 같은 이름만 있는 알림, 알림 구간)는 안 튼다 — 게임 사건으로 남의 캐릭터 능력이 켜질 수 있다
	USkeletalMeshComponent* Mesh = GetSkelMeshComponent();
	for (const FMOBSimpleAnimProxy::FMontageStep& Step : Steps)
	{
		if (!Mesh || !Step.Montage || Step.To <= Step.From) continue;
		for (const FAnimNotifyEvent& Event : Step.Montage->Notifies)
		{
			UAnimNotify* Notify = Event.Notify;
			if (!Notify) continue;
			const float At = Event.GetTriggerTime();
			const bool bInside = (Step.From < 0.f ? At >= 0.f : At > Step.From) && At <= Step.To;
			if (!bInside) continue;
			const bool bWeaponMontage = Notify->GetClass()->GetName().Contains(TEXT("PlayWeaponMontage"));
			if (!bWeaponMontage && !Notify->IsA<UAnimNotify_PlaySound>()) continue;
			Notify->Notify(Mesh, Step.Montage, FAnimNotifyEventReference(&Event, Step.Montage));
			if (!bWeaponMontage)
			{
				++Out.SoundNotifies;
				continue;
			}
			++Out.WeaponNotifies;
			// 알림이 무기를 실제로 움직였나 — 붙은 액터 중 몸과 뼈대가 다른 메시(무기)에 몽타주가 돌고 있으면 센다
			const USkeleton* BodySkeleton = Mesh->GetSkeletalMeshAsset() ? Mesh->GetSkeletalMeshAsset()->GetSkeleton() : nullptr;
			TArray<AActor*> Attached;
			if (AActor* Owner = Mesh->GetOwner()) Owner->GetAttachedActors(Attached, true, true);
			bool bPlaying = false;
			for (const AActor* Actor : Attached)
			{
				TInlineComponentArray<USkeletalMeshComponent*> Meshes(Actor);
				for (const USkeletalMeshComponent* Other : Meshes)
				{
					const USkeletalMesh* OtherAsset = Other->GetSkeletalMeshAsset();
					const UAnimInstance* OtherAnim = Other->GetAnimInstance();
					if (OtherAsset && OtherAsset->GetSkeleton() != BodySkeleton && OtherAnim && OtherAnim->IsAnyMontagePlaying()) bPlaying = true;
				}
			}
			Out.WeaponPlaying += bPlaying ? 1 : 0;
		}
	}
}

void UMOBSimpleAnimInstance::ReadEvaluateResult(FMOBServerAnimResult& Out)
{
	const FMOBSimpleAnimProxy& Proxy = GetProxyOnGameThread<FMOBSimpleAnimProxy>();
	Out.SlotPasses = Proxy.GetSlotPasses();
	Out.bMaskMissing = Proxy.IsMaskMissing();
}

void UMOBSimpleAnimInstance::GetAimJump(float& OutPitch, int32& OutJumpClip, float& OutJumpWeight)
{
	GetProxyOnGameThread<FMOBSimpleAnimProxy>().GetAimJump(OutPitch, OutJumpClip, OutJumpWeight);
}

bool MOBGetSimplePlayers(const ACharacter* Character, FString& Out)
{
	const USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	UMOBSimpleAnimInstance* Instance = Mesh ? Cast<UMOBSimpleAnimInstance>(Mesh->GetAnimInstance()) : nullptr;
	if (!Instance) return false;
	Instance->GetPlayers(Out);
	return true;
}

void MOBAppendSimplePoseState(const ACharacter* Character, FString& Out)
{
	const USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	UMOBSimpleAnimInstance* Instance = Mesh ? Cast<UMOBSimpleAnimInstance>(Mesh->GetAnimInstance()) : nullptr;
	if (Instance)
	{
		Instance->GetPoseState(Out);
		return;
	}
	Out += TEXT(",-1,-1,1,0,0,0,0,0");
	for (int32 I = 0; I < FMOBSimpleAnimProxy::kJumpStack; ++I) Out += TEXT(",-1,0,1");
	Out += TEXT(",-1,-1");
}

bool MOBGetSimpleMoveLog(const ACharacter* Character, FString& Out)
{
	const USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	UMOBSimpleAnimInstance* Instance = Mesh ? Cast<UMOBSimpleAnimInstance>(Mesh->GetAnimInstance()) : nullptr;
	if (!Instance) return false;
	Instance->GetMoveLog(Out);
	return true;
}

void UMOBSimpleAnimInstance::GetMoveLog(FString& Out)
{
	Out = GetProxyOnGameThread<FMOBSimpleAnimProxy>().GetMoveLog();
}

void UMOBSimpleAnimInstance::GetPoseState(FString& Out)
{
	GetProxyOnGameThread<FMOBSimpleAnimProxy>().AppendPoseState(Out);
}

void UMOBSimpleAnimInstance::GetPlayers(FString& Out)
{
	GetProxyOnGameThread<FMOBSimpleAnimProxy>().AppendPlayers(Out);
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
	// 손 붙이기에 쓰는 뼈 — 판정 모양이 없어도 자세 계산에 필요하다
	for (const TCHAR* Name : { HAND_R, IK_GUN, IK_HAND_L, LEFT_ARM[0], LEFT_ARM[1], LEFT_ARM[2] })
	{
		FCompactPoseBoneIndex Index = FindCompactBone(Bones, Name);
		if (!Index.IsValid())
		{
			UE_LOG(LogMOBSimple, Error, TEXT("MOB_SIMPLE_FAIL dump bone=%s"), Name);
			continue;
		}
		while (Index.GetInt() != INDEX_NONE)
		{
			Keep.Add(Index.GetInt());
			if (Index.IsRootBone()) break;
			Index = Bones.GetParentBoneIndex(Index);
		}
	}
	// 이동 상수 — 멈춤 · 돌아서기 거리 예측과 꼭대기 시간에 쓴다. 서버의 Shared/Movement 상수와 대조한다
	const ACharacter* Character = Cast<ACharacter>(GetOwningActor());
	const UCharacterMovementComponent* Move = Character ? Character->GetCharacterMovement() : nullptr;
	if (Move)
	{
		Out += FString::Printf(TEXT("MOVE,groundFriction,%.6f,brakingFriction,%.6f,brakingFrictionFactor,%.6f,separateBraking,%d,brakingDecelerationWalking,%.6f,gravityZ,%.6f,maxWalkSpeed,%.6f\n"),
			Move->GroundFriction, Move->BrakingFriction, Move->BrakingFrictionFactor, Move->bUseSeparateBrakingFriction ? 1 : 0, Move->BrakingDecelerationWalking, Move->GetGravityZ(), Move->MaxWalkSpeed);
	}
	Out += FString::Printf(TEXT("WEAPON,%s\n"), WeaponKey);

	// 몽타주(9단계 3-a) — 뽑을 때만 읽는다. 재생(단순 재생 · 서버 구동)은 몽타주를 안 쓴다
	TArray<const UAnimMontage*> Montages;
	const UAnimSequence* OverridePose = nullptr;
	const FWeaponMontageNames* MontageNames = nullptr;
	for (const FWeaponMontageNames& Names : WEAPON_MONTAGES)
	{
		if (FCString::Strcmp(Names.Key, WeaponKey) == 0) MontageNames = &Names;
	}
	if (!MontageNames) UE_LOG(LogMOBSimple, Error, TEXT("MOB_SIMPLE_FAIL montages weapon=%s — 몽타주 이름 표에 없다"), WeaponKey);
	for (int32 K = 0; MontageNames && K < kMontageKinds; ++K)
	{
		const UAnimMontage* Montage = LoadObject<UAnimMontage>(nullptr, MontageNames->Montages[K]);
		UE_LOG(LogMOBSimple, Log, TEXT("MOB_SIMPLE_MONTAGE kind=%d path=%s ok=%d"), K, MontageNames->Montages[K], Montage ? 1 : 0);
		if (!Montage) UE_LOG(LogMOBSimple, Error, TEXT("MOB_SIMPLE_FAIL montage=%s"), MontageNames->Montages[K]);
		Montages.Add(Montage);
	}
	if (MontageNames)
	{
		OverridePose = LoadObject<UAnimSequence>(nullptr, MontageNames->OverridePose);
		UE_LOG(LogMOBSimple, Log, TEXT("MOB_SIMPLE_MONTAGE override=%s ok=%d"), MontageNames->OverridePose, OverridePose ? 1 : 0);
		if (!OverridePose) UE_LOG(LogMOBSimple, Error, TEXT("MOB_SIMPLE_FAIL override=%s"), MontageNames->OverridePose);
	}
	// 조준 클립(C-3) — 못 읽은 칸은 nullptr 로 넣어 덤프에 CLIPFAIL 로 남긴다(클립 묶는 도구가 거기서 멈춘다)
	TArray<const UAnimSequence*> AdsClips;
	const FWeaponAdsNames* AdsNames = nullptr;
	for (const FWeaponAdsNames& Names : WEAPON_ADS)
	{
		if (FCString::Strcmp(Names.Key, WeaponKey) == 0) AdsNames = &Names;
	}
	if (!AdsNames) UE_LOG(LogMOBSimple, Error, TEXT("MOB_SIMPLE_FAIL ads weapon=%s — 조준 클립 이름 표에 없다"), WeaponKey);
	for (int32 I = 0; I < kAdsClips; ++I)
	{
		const UAnimSequence* Clip = AdsNames ? LoadObject<UAnimSequence>(nullptr, *FString::Printf(TEXT("%s/%s.%s"), AdsNames->Folder, AdsNames->Names[I], AdsNames->Names[I])) : nullptr;
		if (AdsNames && !Clip) UE_LOG(LogMOBSimple, Error, TEXT("MOB_SIMPLE_FAIL ads clip=%s"), AdsNames->Names[I]);
		AdsClips.Add(Clip);
	}
	// 앉기 클립(D-3) — 조준 클립과 같은 방식
	TArray<const UAnimSequence*> CrouchClips;
	const FWeaponAdsNames* CrouchNames = nullptr;
	for (const FWeaponAdsNames& Names : WEAPON_CROUCH)
	{
		if (FCString::Strcmp(Names.Key, WeaponKey) == 0) CrouchNames = &Names;
	}
	if (!CrouchNames) UE_LOG(LogMOBSimple, Error, TEXT("MOB_SIMPLE_FAIL crouch weapon=%s — 앉기 클립 이름 표에 없다"), WeaponKey);
	for (int32 I = 0; I < kCrouchClips; ++I)
	{
		const UAnimSequence* Clip = CrouchNames ? LoadObject<UAnimSequence>(nullptr, *FString::Printf(TEXT("%s/%s.%s"), CrouchNames->Folder, CrouchNames->Names[I], CrouchNames->Names[I])) : nullptr;
		if (CrouchNames && !Clip) UE_LOG(LogMOBSimple, Error, TEXT("MOB_SIMPLE_FAIL crouch clip=%s"), CrouchNames->Names[I]);
		CrouchClips.Add(Clip);
	}
	Proxy.AppendClipDump(Out, SampleHz, Keep, Montages, OverridePose, AdsClips, CrouchClips);
	return true;
}

bool MOBSetServerAnim(ACharacter* Character, const mo::AnimPoseInput& Input, int32 Weapon, const int32* MontageWeapons, const mo::BoneTransform* Local, int32 Count, const TArray<FName>& BoneNames, FMOBServerAnimResult& OutResult)
{
	OutResult = FMOBServerAnimResult();
	USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	UMOBSimpleAnimInstance* Instance = Mesh ? Cast<UMOBSimpleAnimInstance>(Mesh->GetAnimInstance()) : nullptr;
	if (!Instance) return false;
	Instance->SetServerFrame(Input, Weapon, MontageWeapons, Local, Count, BoneNames, OutResult);

	// 다리는 액터 틱이 다 끝난 뒤에 돈다. 여기서 안 당기면 이번 프레임에 그려지는 자세가 위치보다 한 프레임 늦다(쏠 때 보고하는 시점과 어긋난다)
	Mesh->RefreshBoneTransforms();
	Instance->ReadEvaluateResult(OutResult);
	const USkeletalMesh* Asset = Mesh->GetSkeletalMeshAsset();
	const USkeleton* Skeleton = Asset ? Asset->GetSkeleton() : nullptr;
	TArray<AActor*> Attached;
	Character->GetAttachedActors(Attached, true, true);
	for (AActor* Actor : Attached)
	{
		TInlineComponentArray<USkeletalMeshComponent*> Bodies(Actor);
		for (USkeletalMeshComponent* Body : Bodies)
		{
			const USkeletalMesh* BodyAsset = Body->GetSkeletalMeshAsset();
			if (!BodyAsset || BodyAsset->GetSkeleton() != Skeleton || !Body->GetAnimInstance()) continue;	// 무기는 뼈대가 다르고, 리더 자세를 따르는 몸은 위 갱신이 같이 옮긴다
			Body->TickAnimation(0.f, false);
			Body->RefreshBoneTransforms();
		}
	}
	return true;
}

bool MOBStepServerMontages(ACharacter* Character, const mo::AnimPoseInput& Input, uint32 StateTick, const int32* MontageWeapons, bool bContinue, FMOBServerAnimResult& InOutResult)
{
	USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	UMOBSimpleAnimInstance* Instance = Mesh ? Cast<UMOBSimpleAnimInstance>(Mesh->GetAnimInstance()) : nullptr;
	if (!Instance) return false;
	Instance->StepServerMontages(Input, StateTick, MontageWeapons, bContinue, InOutResult);
	return true;
}

// Lyra 의 부품 컴포넌트(ULyraControllerComponent_CharacterParts)는 모듈 밖에 공개되지 않아 직접 부를 수 없다 →
// 블루프린트용으로 열린 함수 RemoveAllCharacterParts · AddCharacterPart 를 이름으로 찾아 부른다
bool MOBSetBodyPart(ACharacter* Character, const TCHAR* PartClassPath)
{
	AController* Ctrl = Character ? Character->GetController() : nullptr;
	UClass* PartsClass = StaticLoadClass(UActorComponent::StaticClass(), nullptr, TEXT("/Script/LyraGame.LyraControllerComponent_CharacterParts"));
	UClass* PartClass = StaticLoadClass(AActor::StaticClass(), nullptr, PartClassPath);
	if (!Ctrl || !PartsClass || !PartClass)
	{
		UE_LOG(LogMOBSimple, Error, TEXT("MOB_SIMPLE_FAIL body ctrl=%d partsClass=%d part=%d path=%s"), Ctrl ? 1 : 0, PartsClass ? 1 : 0, PartClass ? 1 : 0, PartClassPath);
		return false;
	}
	TArray<UActorComponent*> Comps;
	Ctrl->GetComponents(Comps);
	UActorComponent* First = nullptr;
	int32 Found = 0;
	for (UActorComponent* C : Comps)
	{
		if (!C || !C->IsA(PartsClass)) continue;
		++Found;
		if (UFunction* RemoveAll = C->FindFunction(TEXT("RemoveAllCharacterParts"))) C->ProcessEvent(RemoveAll, nullptr);
		if (!First) First = C;
	}
	UFunction* Add = First ? First->FindFunction(TEXT("AddCharacterPart")) : nullptr;
	// 인자는 FLyraCharacterPart 하나다. 크기가 다르면 인자 모양이 바뀐 것이라 부르지 않는다
	if (!Add || Add->ParmsSize != sizeof(FLyraCharacterPart))
	{
		UE_LOG(LogMOBSimple, Error, TEXT("MOB_SIMPLE_FAIL body components=%d add=%d parmsSize=%d expected=%d"), Found, Add ? 1 : 0, Add ? Add->ParmsSize : -1, (int32)sizeof(FLyraCharacterPart));
		return false;
	}
	FLyraCharacterPart Part;
	Part.PartClass = PartClass;
	First->ProcessEvent(Add, &Part);
	UE_LOG(LogMOBSimple, Log, TEXT("MOB_SIMPLE_BODY character=%s part=%s components=%d"), *GetNameSafe(Character), *GetNameSafe(PartClass), Found);
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
