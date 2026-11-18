#include "MOBAnimCompareSubsystem.h"

#include "AIController.h"
#include "AbilitySystem/LyraAbilitySystemComponent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Abilities/GameplayAbility.h"
#include "Animation/AnimClassInterface.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/BlendProfile.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimNode_StateMachine.h"
#include "Animation/AnimStateMachineTypes.h"
#include "BrainComponent.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"
#include "Camera/CameraActor.h"
#include "Character/LyraCharacter.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameEngine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Equipment/LyraEquipmentDefinition.h"
#include "Equipment/LyraEquipmentInstance.h"
#include "Equipment/LyraEquipmentManagerComponent.h"
#include "Equipment/LyraQuickBarComponent.h"
#include "FrameGrabber.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Inventory/LyraInventoryItemDefinition.h"
#include "Inventory/LyraInventoryItemInstance.h"
#include "Inventory/LyraInventoryManagerComponent.h"
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
	// 경로 안의 사건(-MOBACScript=events). Fire = 방아쇠를 잠깐(FIRE_TAP_SECONDS) 당긴다 · Hold/Release = 당긴 채 두기/놓기
	// Swap = 다른 무기로 바꾼다(빠른 칸을 바꾼다 — 까닭은 EquipFoeWeapon 주석) · Back = 처음 무기로
	// Spawn = 부활 효과 능력(GA_SpawnEffect)을 다시 켠다 — 봇을 죽였다 살리면 몸이 바뀌어 이 도구가 끊기므로 효과만 다시 튼다
	// AdsOn · AdsOff = 조준을 누른다 · 뗀다(-MOBACScript=ads)
	// CrouchOn · CrouchOff = 앉기 단추를 눌러 앉는다 · 일어선다(-MOBACScript=crouch). Lyra 단추는 누를 때마다 뒤집으므로(ToggleCrouch) 이미 그 상태면 안 누른다
	// Melee = 근접 단추 · HitBack · HitFront = 맞은 쪽 반응 몽타주를 피해 연출과 같은 길로 튼다(-MOBACScript=melee)
	enum class EACEvent : uint8 { None, Fire, Hold, Release, Reload, Swap, Back, Spawn, AdsOn, AdsOff, CrouchOn, CrouchOff, Melee, HitBack, HitFront };

	// 경로 한 조각. Angle = 몸 앞 기준 이동 방향(+90 오른쪽). AngleRate·YawRate = 초당 돌리는 각도
	// PitchFrom → PitchTo = 조각 동안 시선 위아래 각(+ 위). bJump = 조각 처음에 뛴다. Ev0·Ev1 = 조각 시작에서 At0·At1 초에 일으킬 사건
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
		EACEvent Ev0 = EACEvent::None;
		float At0 = 0.f;
		EACEvent Ev1 = EACEvent::None;
		float At1 = 0.f;
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

	// 9단계 사건 경로(-MOBACScript=events, candidate1-step9-plan.md 9-1). 조각 길이 = 그 사건의 몽타주가 다 빠질 만큼(가장 긴 재장전 2.833초 + 빼기 0.35초)에 여유를 둔 제안값.
	// fire_b = 재장전 직후라 탄창이 차 있어 다음 재장전이 안 걸릴 수 있어 한 발 쓴다. 위아래 각 ±45 = aimjump 의 달리며 겨눔과 같은 폭
	// 달리는 조각 셋은 startstop 경로가 이미 밟은 바닥 안에 머문다(animcmp_b0_startstop.csv 실측 = x -2782..-1020 · y -2834..353, 높이 142 그대로, 2.5초에 1,350cm).
	// 앞으로 1.8초(→ x ≈ -2026, startstop 이 좌우로 달린 줄) · 오른쪽 2.5초 · 왼쪽 2.5초로 제자리 줄에 돌아온다.
	// 처음 판(11-12)은 셋 다 앞으로 7.5초를 달려 바닥 밖으로 떨어졌다(run_reload fallFrames=93) — 그래서 이렇게 나눴다. 재장전은 달리기 2.5초 + 멈춤 1.5초에 걸쳐 끝난다
	// 당겨 쏘기 0.5초 = 권총은 당긴 채면 8프레임마다 다시 쏴(셋째 시험 1초에 9발) 재장전 조각 전에 탄창이 비고, 그러면 Lyra 가 스스로 재장전해 경로의 재장전이 헛돌았다. 반으로 줄였다(ammo 칸으로 확인)
	using ACE = EACEvent;
	const FScriptSegment EVENTS_SCRIPT[] = {
		{ TEXT("idle"),          2.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false },
		{ TEXT("fire_stand"),    1.5f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::Fire, 0.f },
		{ TEXT("fire_hold"),     2.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::Hold, 0.f, ACE::Release, 0.5f },
		{ TEXT("run_fire"),      1.8f, true,    0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::Fire, 0.3f, ACE::Fire, 1.0f },
		{ TEXT("stop_f"),        1.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false },
		{ TEXT("up_fire"),       1.5f, false,   0.f, 0.f, 0.f,   0.f,  45.f, false, ACE::Fire, 1.0f },
		{ TEXT("down_fire"),     1.5f, false,   0.f, 0.f, 0.f,  45.f, -45.f, false, ACE::Fire, 1.0f },
		{ TEXT("mid"),           1.0f, false,   0.f, 0.f, 0.f, -45.f,   0.f, false },
		{ TEXT("reload"),        3.5f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::Reload, 0.f },
		{ TEXT("fire_b"),        1.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::Fire, 0.f },
		{ TEXT("reload_cancel"), 2.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::Reload, 0.f, ACE::Fire, 0.6f },
		{ TEXT("settle_c"),      1.5f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false },
		{ TEXT("run_reload"),    2.5f, true,   90.f, 0.f, 0.f,   0.f,   0.f, false, ACE::Reload, 0.2f },
		{ TEXT("stop_r"),        1.5f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false },
		{ TEXT("swap_out"),      2.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::Swap, 0.f },
		{ TEXT("swap_back"),     2.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::Back, 0.f },
		{ TEXT("swap_fire"),     1.5f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::Swap, 0.f, ACE::Fire, 0.25f },
		{ TEXT("swap_back2"),    2.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::Back, 0.f },
		{ TEXT("run_swap"),      2.5f, true,  -90.f, 0.f, 0.f,   0.f,   0.f, false, ACE::Swap, 0.4f, ACE::Back, 1.4f },
		{ TEXT("stop_s"),        1.5f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false },
		{ TEXT("spawn"),         3.5f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::Spawn, 0.f },
		{ TEXT("idle_end"),      1.5f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false },
	};
	constexpr int32 NUM_EVENTS_SEGMENTS = UE_ARRAY_COUNT(EVENTS_SCRIPT);

	// 조준 경로(-MOBACScript=ads, content-add-progress.md C-3). 조준 자세가 바뀌는 자리를 한 번씩 —
	// 서서 켜고 끔(서기 클립 바꿈) · 조준 중 위아래 · 조준 걸음 네 방향 출발 · 멈춤 · 돌아서기(돌아서기 클립 방향 = 돌기 전 걸음 방향이라 앞뒤 · 뒤앞 · 좌우 · 우좌 넷) · 조준 점프 ·
	// 달리다 끄고 켬(반복 클립 바꿈) · 출발 · 멈춤 · 돌아서기 도중 바꿈(Lyra 는 그 상태를 바로 빠진다, ABP_Mannequin_Base 전환 9 · 10 · 23).
	// 길이 = startstop 경로와 같은 몫(출발 2.5 · 멈춤 2.0 · 돌아서기 1.5초). 조준 중 걸음은 300cm/s 라 같은 시간에 절반만 간다.
	// 달리다 바꾸는 둘(run_off · run_on)은 앞 · 뒤로 같은 거리라 제자리 줄로 돌아오고, 옆 셋 뒤 ret_left 가 오른쪽으로 간 몫(≈1,200cm)을 되돌린다(계산값)
	const FScriptSegment ADS_SCRIPT[] = {
		{ TEXT("idle"),          2.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false },
		{ TEXT("ads_on"),        1.5f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::AdsOn, 0.f },
		{ TEXT("ads_up"),        1.5f, false,   0.f, 0.f, 0.f,   0.f,  45.f, false },
		{ TEXT("ads_down"),      1.5f, false,   0.f, 0.f, 0.f,  45.f, -45.f, false },
		{ TEXT("ads_mid"),       1.0f, false,   0.f, 0.f, 0.f, -45.f,   0.f, false },
		{ TEXT("ads_off"),       1.5f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::AdsOff, 0.f },
		{ TEXT("ads_on2"),       1.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::AdsOn, 0.f },
		{ TEXT("a_fwd"),         2.5f, true,    0.f, 0.f, 0.f },
		{ TEXT("a_stop_fwd"),    2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("a_bwd"),         2.5f, true,  180.f, 0.f, 0.f },
		{ TEXT("a_stop_bwd"),    2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("a_left"),        2.5f, true,  -90.f, 0.f, 0.f },
		{ TEXT("a_stop_left"),   2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("a_right"),       2.5f, true,   90.f, 0.f, 0.f },
		{ TEXT("a_stop_right"),  2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("a_piv_bf_a"),    1.5f, true,  180.f, 0.f, 0.f },
		{ TEXT("a_piv_bf_b"),    1.5f, true,    0.f, 0.f, 0.f },
		{ TEXT("a_stop_bf"),     2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("a_piv_fb_a"),    1.5f, true,    0.f, 0.f, 0.f },
		{ TEXT("a_piv_fb_b"),    1.5f, true,  180.f, 0.f, 0.f },
		{ TEXT("a_stop_fb"),     2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("a_piv_lr_a"),    1.5f, true,  -90.f, 0.f, 0.f },
		{ TEXT("a_piv_lr_b"),    1.5f, true,   90.f, 0.f, 0.f },
		{ TEXT("a_stop_lr"),     2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("a_piv_rl_a"),    1.5f, true,   90.f, 0.f, 0.f },
		{ TEXT("a_piv_rl_b"),    1.5f, true,  -90.f, 0.f, 0.f },
		{ TEXT("a_stop_rl"),     2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("a_jump"),        1.6f, false,   0.f, 0.f, 0.f,   0.f,   0.f, true  },
		{ TEXT("a_settle"),      1.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("run_off"),       2.5f, true,    0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::AdsOff, 1.2f },
		{ TEXT("run_on"),        2.5f, true,  180.f, 0.f, 0.f,   0.f,   0.f, false, ACE::AdsOn, 1.2f },
		{ TEXT("stop_a"),        2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("start_toggle"),  1.5f, true,   90.f, 0.f, 0.f,   0.f,   0.f, false, ACE::AdsOff, 0.3f },
		{ TEXT("stop_toggle"),   2.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::AdsOn, 0.3f },
		{ TEXT("piv_toggle_a"),  1.5f, true,  -90.f, 0.f, 0.f },
		{ TEXT("piv_toggle_b"),  1.5f, true,   90.f, 0.f, 0.f,   0.f,   0.f, false, ACE::AdsOff, 0.2f },
		{ TEXT("ret_left"),      2.0f, true,  -90.f, 0.f, 0.f },
		{ TEXT("stop_end"),      2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("idle_end"),      1.5f, false,   0.f, 0.f, 0.f },
	};
	constexpr int32 NUM_ADS_SEGMENTS = UE_ARRAY_COUNT(ADS_SCRIPT);

	// 오래 조준한 채 서 있다 풀기(-MOBACScript=adslong, content-add-progress.md 미결 (마)) — 허리 서기 클립 길이(소총 6.67초)를 넘긴 시각에 조준을 뗀다.
	// 서기 시계는 하나이고 지금 쪽 서기 클립 길이로 접힌다(소총 허리 6.667 · 조준 8.0초, 11-18 adsl_rifle 기록). 그래서 「시계가 허리 길이를 넘은 채 뗀다」는 시계가 6.667~8.0초일 때만 생긴다.
	// 첫 조준 8초 = 조준 길이로 한 바퀴(뗄 때 시계 4.8 — 허리 길이 안). 둘째 조준 6.1초 = 같은 기록의 판 시작 시계(0판 2.883 · 1판 2.767)로 셈하면 뗄 때 7.35 · 7.23초 — 칸 양끝에서 0.56초 넘게 떨어진다(계산값).
	// 권총은 그 칸이 8.733~9.033초뿐이라 이 각본으로는 안 밟는다(엔진 규칙은 무기와 무관). 뗀 뒤 3초는 대기 몸짓 시계(멈춘 뒤 6초 + 멈춤 클립, AnimState kIdleBreakDelay)가 다 차기 전이라 몸짓 없이 허리 서기만 본다
	const FScriptSegment ADSLONG_SCRIPT[] = {
		{ TEXT("idle"),          2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("ads_hold"),      8.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::AdsOn, 0.f },
		{ TEXT("ads_release"),   3.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::AdsOff, 0.f },
		{ TEXT("ads_hold2"),     6.1f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::AdsOn, 0.f },
		{ TEXT("ads_release2"),  3.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::AdsOff, 0.f },
	};
	constexpr int32 NUM_ADSLONG_SEGMENTS = UE_ARRAY_COUNT(ADSLONG_SCRIPT);

	// 앉기 경로(-MOBACScript=crouch, content-add-progress.md D-3). ads 경로와 같은 짜임에 조준 누름을 앉기 누름으로 바꿨다. 앉음이 바뀌는 자리를 한 번씩 —
	// 서서 앉고 일어섬(서기 상태 안 전환 클립 Crouch_Idle_Entry · _Exit) · 앉아 위아래 · 앉은 걸음 네 방향 출발 · 멈춤 · 돌아서기 넷 · 앉아 쏘기(쏜 뒤 덮기 자세의 앉은 판) ·
	// 앉아 조준(서기 클립은 앉음이 조준보다 먼저) · 앉은 데서 점프(점프 능력처럼 일어선 뒤 뛴다, DriveFoe) · 달리기 · 출발 · 멈춤 · 돌아서기 도중 앉음 바꿈(ABP_Mannequin_Base 전환 9 · 10 · 23 은 CrouchStateChange 를 조준 바뀜과 같은 자리에 둔다).
	// 길이 = ads 경로와 같은 몫. 앉은 걸음 300cm/s(MaxWalkSpeedCrouched)는 조준 걸음과 같은 빠르기라 ads 경로가 밟은 바닥 안에 머문다. 앉아 조준 걸음은 앞으로 2초 · 뒤로 2초로 돌아온다.
	// 달리다 바꾸는 둘은 앞 ≈1,110 · 뒤 ≈1,140cm 로 제자리 줄에 돌아오고, 옆 셋(오른쪽 ≈540 · 왼쪽 ≈900 · 오른쪽 ≈510) 뒤 ret_left 0.5초가 남은 ≈150cm 를 되돌린다(계산값, 가속 뺌).
	// 마지막 idle_end 2초 = 일어서기 전환 클립이 서기 안에서 끝나도록 c_off 보다 길게(제안값)
	const FScriptSegment CROUCH_SCRIPT[] = {
		{ TEXT("idle"),          2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("c_on"),          1.5f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::CrouchOn, 0.f },
		{ TEXT("c_up"),          1.5f, false,   0.f, 0.f, 0.f,   0.f,  45.f, false },
		{ TEXT("c_down"),        1.5f, false,   0.f, 0.f, 0.f,  45.f, -45.f, false },
		{ TEXT("c_mid"),         1.0f, false,   0.f, 0.f, 0.f, -45.f,   0.f, false },
		{ TEXT("c_off"),         1.5f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::CrouchOff, 0.f },
		{ TEXT("c_on2"),         1.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::CrouchOn, 0.f },
		{ TEXT("c_fwd"),         2.5f, true,    0.f, 0.f, 0.f },
		{ TEXT("c_stop_fwd"),    2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("c_bwd"),         2.5f, true,  180.f, 0.f, 0.f },
		{ TEXT("c_stop_bwd"),    2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("c_left"),        2.5f, true,  -90.f, 0.f, 0.f },
		{ TEXT("c_stop_left"),   2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("c_right"),       2.5f, true,   90.f, 0.f, 0.f },
		{ TEXT("c_stop_right"),  2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("c_piv_bf_a"),    1.5f, true,  180.f, 0.f, 0.f },
		{ TEXT("c_piv_bf_b"),    1.5f, true,    0.f, 0.f, 0.f },
		{ TEXT("c_stop_bf"),     2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("c_piv_fb_a"),    1.5f, true,    0.f, 0.f, 0.f },
		{ TEXT("c_piv_fb_b"),    1.5f, true,  180.f, 0.f, 0.f },
		{ TEXT("c_stop_fb"),     2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("c_piv_lr_a"),    1.5f, true,  -90.f, 0.f, 0.f },
		{ TEXT("c_piv_lr_b"),    1.5f, true,   90.f, 0.f, 0.f },
		{ TEXT("c_stop_lr"),     2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("c_piv_rl_a"),    1.5f, true,   90.f, 0.f, 0.f },
		{ TEXT("c_piv_rl_b"),    1.5f, true,  -90.f, 0.f, 0.f },
		{ TEXT("c_stop_rl"),     2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("c_fire"),        1.5f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::Fire, 0.3f },
		{ TEXT("c_ads_on"),      1.5f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::AdsOn, 0.f },
		{ TEXT("c_ads_fwd"),     2.0f, true,    0.f, 0.f, 0.f },
		{ TEXT("c_ads_stop"),    2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("c_ads_bwd"),     2.0f, true,  180.f, 0.f, 0.f },
		{ TEXT("c_ads_off"),     2.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::AdsOff, 0.5f },
		{ TEXT("c_jump"),        1.6f, false,   0.f, 0.f, 0.f,   0.f,   0.f, true  },
		{ TEXT("c_settle"),      1.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("run_on"),        2.5f, true,    0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::CrouchOn, 1.2f },
		{ TEXT("run_off"),       2.5f, true,  180.f, 0.f, 0.f,   0.f,   0.f, false, ACE::CrouchOff, 1.2f },
		{ TEXT("stop_a"),        2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("start_toggle"),  1.5f, true,   90.f, 0.f, 0.f,   0.f,   0.f, false, ACE::CrouchOn, 0.3f },
		{ TEXT("stop_toggle"),   2.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::CrouchOff, 0.3f },
		{ TEXT("piv_toggle_a"),  1.5f, true,  -90.f, 0.f, 0.f },
		{ TEXT("piv_toggle_b"),  1.5f, true,   90.f, 0.f, 0.f,   0.f,   0.f, false, ACE::CrouchOn, 0.2f },
		{ TEXT("ret_left"),      0.5f, true,  -90.f, 0.f, 0.f },
		{ TEXT("stop_end"),      2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("idle_end"),      2.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::CrouchOff, 0.f },
	};
	constexpr int32 NUM_CROUCH_SEGMENTS = UE_ARRAY_COUNT(CROUCH_SCRIPT);

	// 근접 경로(-MOBACScript=melee, content-add-progress.md E-2). 근접 · 맞은 쪽 반응 몽타주가 몸에 얹히는 자리를 한 번씩 —
	// 서서 근접 둘(조각 2초 > 연타 간격 샷건 0.77초) · 달리며 근접 · 멈추는 중 근접 · 반응 뒤 · 앞 · 달리며 반응 · 멈추는 중 반응 ·
	// 근접 중 반응 · 반응 중 근접(같은 무리면 앞 몽타주가 빠진다 — 몽타주 표의 무리 칸) · 반응 중 쏘기 · 부활 중 반응(통째 자리가 반응 뒤에 덮는지) · 앉아 근접 · 앉아 반응.
	// 길이 = 몽타주가 다 빠질 만큼(반응 0.87초 + 빼기)에 여유를 둔 제안값. 달리는 둘은 앞 1.8 · 뒤 1.8초로 제자리 줄에 돌아온다(events 경로 run_fire 와 같은 몫)
	const FScriptSegment MELEE_SCRIPT[] = {
		{ TEXT("idle"),          2.0f, false,   0.f, 0.f, 0.f },
		{ TEXT("melee"),         2.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::Melee, 0.f },
		{ TEXT("melee2"),        2.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::Melee, 0.f },
		{ TEXT("run_melee"),     1.8f, true,    0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::Melee, 0.4f },
		{ TEXT("stop_melee"),    2.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::Melee, 0.1f },
		{ TEXT("hit_back"),      2.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::HitBack, 0.f },
		{ TEXT("hit_front"),     2.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::HitFront, 0.f },
		{ TEXT("run_hit"),       1.8f, true,  180.f, 0.f, 0.f,   0.f,   0.f, false, ACE::HitBack, 0.4f },
		{ TEXT("stop_hit"),      2.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::HitFront, 0.1f },
		{ TEXT("melee_hit"),     2.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::Melee, 0.f, ACE::HitBack, 0.2f },
		{ TEXT("hit_melee"),     2.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::HitFront, 0.f, ACE::Melee, 0.2f },
		{ TEXT("hit_fire"),      2.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::HitBack, 0.f, ACE::Fire, 0.3f },
		{ TEXT("spawn_hit"),     3.5f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::Spawn, 0.f, ACE::HitFront, 0.5f },
		{ TEXT("c_on"),          1.5f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::CrouchOn, 0.f },
		{ TEXT("c_melee"),       2.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::Melee, 0.f },
		{ TEXT("c_hit"),         2.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::HitBack, 0.f },
		{ TEXT("idle_end"),      2.0f, false,   0.f, 0.f, 0.f,   0.f,   0.f, false, ACE::CrouchOff, 0.f },
	};
	constexpr int32 NUM_MELEE_SEGMENTS = UE_ARRAY_COUNT(MELEE_SCRIPT);
	const TCHAR* const HIT_BACK_PATH = TEXT("/Game/Characters/Heroes/Mannequin/Animations/Actions/AM_MM_HitReact_Back_Med_01.AM_MM_HitReact_Back_Med_01");
	const TCHAR* const HIT_FRONT_PATH = TEXT("/Game/Characters/Heroes/Mannequin/Animations/Actions/AM_MM_HitReact_Front_Med_02.AM_MM_HitReact_Front_Med_02");
	constexpr float FIRE_TAP_SECONDS = 0.05f;	// 한 발 사건의 방아쇠 시간 — 3프레임. 연사 소총이 한 발만 내도록 짧게(제안값)
	constexpr int32 NUM_FOE_WEAPONS = 3;	// 상대 빠른 칸 0 권총 · 1 소총 · 2 샷건
	const TCHAR* const WEAPON_KEYS[3] = { TEXT("pistol"), TEXT("rifle"), TEXT("shotgun") };
	const TCHAR* const WEAPON_DIRS[3] = { TEXT("Pistol"), TEXT("Rifle"), TEXT("Shotgun") };
	const TCHAR* const SPAWN_EFFECT_PATH = TEXT("/ShooterCore/Game/Respawn/GA_SpawnEffect.GA_SpawnEffect_C");
	const TCHAR* const UPPER_LOWER_MASK = TEXT("UpperBodyLowerBodySplitMask");	// 골격 안의 이름(자산 문자열 기준)
	constexpr double AIM_MARK_DISTANCE = 1000.0;	// 시선 표적을 둘 거리. 멀수록 상대가 움직여도 각이 덜 흔들린다
	constexpr float JUMP_PRESS_SECONDS = 0.1f;		// 뛰기 입력을 누르고 있는 시간 — 계속 누르면 내려앉자마자 또 뛴다

	const FScriptSegment& Seg(EMOBACScript Kind, int32 I)
	{
		switch (Kind)
		{
		case EMOBACScript::AimJump: return AIMJUMP_SCRIPT[I];
		case EMOBACScript::StartStop: return STARTSTOP_SCRIPT[I];
		case EMOBACScript::Events: return EVENTS_SCRIPT[I];
		case EMOBACScript::Ads: return ADS_SCRIPT[I];
		case EMOBACScript::AdsLong: return ADSLONG_SCRIPT[I];
		case EMOBACScript::Crouch: return CROUCH_SCRIPT[I];
		case EMOBACScript::Melee: return MELEE_SCRIPT[I];
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
		case EMOBACScript::Events: return NUM_EVENTS_SEGMENTS;
		case EMOBACScript::Ads: return NUM_ADS_SEGMENTS;
		case EMOBACScript::AdsLong: return NUM_ADSLONG_SEGMENTS;
		case EMOBACScript::Crouch: return NUM_CROUCH_SEGMENTS;
		case EMOBACScript::Melee: return NUM_MELEE_SEGMENTS;
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
	Script = ScriptName == TEXT("aimjump") ? EMOBACScript::AimJump : ScriptName == TEXT("startstop") ? EMOBACScript::StartStop
		: ScriptName == TEXT("events") ? EMOBACScript::Events : ScriptName == TEXT("ads") ? EMOBACScript::Ads
		: ScriptName == TEXT("crouch") ? EMOBACScript::Crouch : ScriptName == TEXT("melee") ? EMOBACScript::Melee
		: ScriptName == TEXT("adslong") ? EMOBACScript::AdsLong : EMOBACScript::Run;
	bAimJump = Script != EMOBACScript::Run;
	FString WeaponName = WEAPON_KEYS[1];
	FParse::Value(Cmd, TEXT("MOBACWeapon="), WeaponName);
	WeaponIndex = WeaponName == WEAPON_KEYS[0] ? 0 : WeaponName == WEAPON_KEYS[2] ? 2 : 1;
	HeldWeapon = WeaponIndex;
	FParse::Value(Cmd, TEXT("MOBACEventDelay="), EventDelay);
	FParse::Value(Cmd, TEXT("MOBACVideoPass="), VideoPass);
	bProbe = FParse::Param(Cmd, TEXT("MOBACProbe"));
	bNoPostProcess = FParse::Param(Cmd, TEXT("MOBACNoPostProcess"));
	if (FParse::Param(Cmd, TEXT("MOBACNoControlRig")))
	{
		// 엔진 AnimNode_ControlRigBase::CanExecute 가 이 값을 본다 — 켜진 값을 읽어 로그에 남겨 실제로 걸렸는지 본다
		IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("ControlRig.DisableExecutionInAnimNode"));
		if (Var) Var->Set(1, ECVF_SetByCode);
		UE_LOG(LogMOBAComp, Log, TEXT("MOB_ACOMP_NOCONTROLRIG found=%d value=%d"), Var ? 1 : 0, Var ? Var->GetInt() : -1);
	}
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

	// 무기를 쥐여 준다 — 기본은 소총(단순 재생의 클립이 소총 것이다). -MOBACWeapon 으로 권총 · 샷건(9단계 Lyra 원본 기록)
	// 상대 조종기의 가방 · 빠른 칸에 무기 셋을 0 · 1 · 2 번에 넣고 칸을 고른다(무기 기록 도구 UMOBWeaponRecordSubsystem::AddWeapons 와 같은 방식).
	// 장비 칸에 바로 넣으면 장비에 가방 아이템이 안 붙어 탄약 비용 확인(LyraAbilityCost_ItemTagStack::CheckCost)이 늘 거짓이라 쏘기 · 재장전이 안 켜진다 — 11-12 첫 시험에서 쏘기 · 재장전 몽타주가 0개였다.
	// 칸 바꾸기도 장비는 EquipItem 으로 넣고 그 뒤에 아이템을 붙인다(LyraQuickBarComponent::EquipItemInSlot) — 꺼내기 몽타주를 트는 자리는 장비 칸에 바로 넣을 때와 같다
	ULyraEquipmentManagerComponent* EM = Target->FindComponentByClass<ULyraEquipmentManagerComponent>();
	AController* FoeCtrl = Target->GetController();
	ULyraInventoryManagerComponent* Inv = FoeCtrl ? FoeCtrl->FindComponentByClass<ULyraInventoryManagerComponent>() : nullptr;
	ULyraQuickBarComponent* QB = FoeCtrl ? FoeCtrl->FindComponentByClass<ULyraQuickBarComponent>() : nullptr;
	if (!EM || !Inv || !QB)
	{
		UE_LOG(LogMOBAComp, Error, TEXT("MOB_ACOMP_FAIL weapon=%s em=%d inv=%d qb=%d"), WEAPON_KEYS[WeaponIndex], EM ? 1 : 0, Inv ? 1 : 0, QB ? 1 : 0);
		return false;
	}
	const int32 OldCount = EM->GetEquipmentInstancesOfType(ULyraEquipmentInstance::StaticClass()).Num();
	for (int32 I = 0; I < NUM_FOE_WEAPONS; ++I) QB->RemoveItemFromSlot(I);		// 고른 칸이면 장비도 뺀다
	for (ULyraEquipmentInstance* I : EM->GetEquipmentInstancesOfType(ULyraEquipmentInstance::StaticClass())) EM->UnequipItem(I);	// 칸 밖에서 들어온 장비가 남았으면
	for (int32 I = 0; I < NUM_FOE_WEAPONS; ++I)
	{
		const FString ItemPath = FString::Printf(TEXT("/ShooterCore/Weapons/%s/ID_%s.ID_%s_C"), WEAPON_DIRS[I], WEAPON_DIRS[I], WEAPON_DIRS[I]);
		UClass* ItemClass = LoadClass<ULyraInventoryItemDefinition>(nullptr, *ItemPath);
		ULyraInventoryItemInstance* Item = ItemClass ? Inv->AddItemDefinition(ItemClass, 1) : nullptr;
		if (!Item)
		{
			UE_LOG(LogMOBAComp, Error, TEXT("MOB_ACOMP_FAIL item=%s class=%d"), *ItemPath, ItemClass ? 1 : 0);
			return false;
		}
		QB->AddItemToSlot(I, Item);
	}
	QB->SetActiveSlotIndex(WeaponIndex);
	const ULyraEquipmentInstance* Rifle = EM->GetFirstInstanceOfType(ULyraEquipmentInstance::StaticClass());
	if (QB->GetActiveSlotIndex() != WeaponIndex || !Rifle)
	{
		UE_LOG(LogMOBAComp, Error, TEXT("MOB_ACOMP_FAIL weapon=%s slot=%d equipped=%d"), WEAPON_KEYS[WeaponIndex], QB->GetActiveSlotIndex(), Rifle ? 1 : 0);
		return false;
	}
	HeldWeapon = WeaponIndex;

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
	if (bNoPostProcess && Target->GetMesh())
	{
		Target->GetMesh()->SetDisablePostProcessBlueprint(true);
		UE_LOG(LogMOBAComp, Log, TEXT("MOB_ACOMP_NOPOSTPROCESS disabled=%d"), Target->GetMesh()->GetDisablePostProcessBlueprint() ? 1 : 0);
	}
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
			*Tag, *GetNameSafe(Target), *GetNameSafe(Rifle), OldCount, PadsHidden, Home.X, Home.Y, Home.Z, BodyYaw);
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

	UE_LOG(LogMOBAComp, Log, TEXT("MOB_ACOMP ready tag=%s simple=%d video=%d foe=%s rifle=%s weapon=%s removedWeapons=%d pads=%d home=(%.0f,%.0f,%.0f) yaw=%.1f"),
		*Tag, bSimple ? 1 : 0, bVideo ? 1 : 0, *GetNameSafe(Target), *GetNameSafe(Rifle), WEAPON_KEYS[WeaponIndex], OldCount, PadsHidden, Home.X, Home.Y, Home.Z, BodyYaw);
	if (bProbe) WriteBlendProfiles();
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
	SetFoeAdsReleased();	// 앞 판이 조준 중에 끝났어도 새 판은 뗀 채로 시작한다
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
			ProbeVars.Add({ Inst, *It, Inst->GetClass(), Count });
		}
		if (const IAnimClassInterface* AC = IAnimClassInterface::GetFromClass(Inst->GetClass()))
		{
			const TArray<FBakedAnimationStateMachine>& Machines = AC->GetBakedStateMachines();
			for (int32 M = 0; M < Machines.Num(); ++M)
			{
				MachineHeader += FString::Printf(TEXT(",sm:%s.%s"), *InstName, *Machines[M].MachineName.ToString().Replace(TEXT(","), TEXT(" ")));
				ProbeMachines.Add({ Inst, Machines[M].MachineName, Inst->GetClass() });
			}
		}
	});
	// 줄은 변수 전부 → 상태 기계 전부 차례로 쓴다(WriteProbe). 제목도 같은 차례로
	// events · montages · weapon · reloadTag · ammo = 9단계 사건 경로용(다른 경로에서는 events 가 비고 montages 는 대개 빈다)
	Header += MachineHeader + TEXT(",montage,players,curves,events,montages,weapon,reloadTag,ammo\n");
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
	// 몸에서 떨어진 인스턴스(무기를 바꿔 갈린 무기 층)는 같은 클래스의 지금 인스턴스로 다시 붙인다. 떨어진 옛것은 바로 안 지워져 IsValid 로는 못 가른다 — 지금 몸에 붙은 것과 같은지로 본다
	TMap<const UClass*, UAnimInstance*> LiveByClass;
	const_cast<USkeletalMeshComponent*>(Mesh)->ForEachAnimInstance([&LiveByClass](UAnimInstance* Inst) { LiveByClass.Add(Inst->GetClass(), Inst); });
	const auto Rebind = [this, &LiveByClass](TWeakObjectPtr<UAnimInstance>& Inst, const TWeakObjectPtr<UClass>& Class)
	{
		UAnimInstance* Now = LiveByClass.FindRef(Class.Get());
		if (Now == Inst.Get()) return;
		Inst = Now;		// 없으면 비워 빈 칸을 쓴다
		if (Now) ++ProbeRebinds;
	};
	bool bBlank = false;
	for (FProbeVar& Var : ProbeVars)
	{
		Rebind(Var.Instance, Var.Class);
		const UAnimInstance* Inst = Var.Instance.Get();
		double Value[3] = {};
		const int32 Count = Inst ? ReadProbeProperty(Var.Prop, Inst, Value) : 0;
		if (Count == 3) Row += FString::Printf(TEXT(",%.4g,%.4g,%.4g"), Value[0], Value[1], Value[2]);
		else if (Count == 1) Row += FString::Printf(TEXT(",%.4g"), Value[0]);
		else
		{
			Row += Var.Columns == 3 ? TEXT(",,,") : TEXT(",");	// 못 읽음 = 빈 칸(0 과 가른다)
			bBlank = true;
		}
	}
	if (bBlank) ++ProbeBlankRows;
	for (FProbeMachine& M : ProbeMachines)
	{
		Rebind(M.Instance, M.Class);
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
	Row += TEXT(",") + Curves;
	// 재생 중 몽타주 전부 — 「이름#번호@위치*지금 비중>목표 비중 r재생 속도 b섞기 시간;」. 목표 0 = 빠지는 중. 위치는 1/6000초보다 잘게 남긴다
	FString Montages;
	if (Main)
	{
		NoteMontageAssets();
		for (const FAnimMontageInstance* I : Main->MontageInstances)
		{
			if (!I || !I->Montage) continue;
			Montages += FString::Printf(TEXT("%s#%d@%.5f*%.4f>%.2f r%.3f b%.3f;"), *I->Montage->GetName(), I->GetInstanceID(), I->GetPosition(), I->GetWeight(), I->GetDesiredWeight(), I->GetPlayRate(), I->GetBlendTime());
		}
	}
	const UAbilitySystemComponent* FoeAsc = FindASC(const_cast<ACharacter*>(Target));
	const int32 ReloadTag = FoeAsc ? FoeAsc->GetGameplayTagCount(FGameplayTag::RequestGameplayTag(TEXT("Event.Movement.Reload"))) : -1;
	// 쥔 무기 탄창 — 「탄창/탄창 크기/남은 탄」(태그 이름 = ID_Pistol 자산 문자열 기준). 탄창이 비면 Lyra 가 스스로 재장전해 경로의 재장전 사건이 헛돈다(11-12 셋째 시험)
	FString Ammo;
	const AController* FoeCtrl = Target->GetController();
	const ULyraQuickBarComponent* QB = FoeCtrl ? FoeCtrl->FindComponentByClass<ULyraQuickBarComponent>() : nullptr;
	if (const ULyraInventoryItemInstance* Item = QB ? QB->GetActiveSlotItem() : nullptr)
	{
		static const FGameplayTag MagTag = FGameplayTag::RequestGameplayTag(TEXT("Lyra.ShooterGame.Weapon.MagazineAmmo"));
		static const FGameplayTag SizeTag = FGameplayTag::RequestGameplayTag(TEXT("Lyra.ShooterGame.Weapon.MagazineSize"));
		static const FGameplayTag SpareTag = FGameplayTag::RequestGameplayTag(TEXT("Lyra.ShooterGame.Weapon.SpareAmmo"));
		Ammo = FString::Printf(TEXT("%d/%d/%d"), Item->GetStatTagStackCount(MagTag), Item->GetStatTagStackCount(SizeTag), Item->GetStatTagStackCount(SpareTag));
	}
	Row += FString::Printf(TEXT(",%s,%s,%s,%d,%s\n"), *FrameEvents, *Montages, WEAPON_KEYS[HeldWeapon], ReloadTag, *Ammo);
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
		if (S.bJump && SegmentTime < JUMP_PRESS_SECONDS)
		{
			if (!Target->bPressedJump) Target->UnCrouch();	// 점프 능력과 같은 차례 — 처음 누른 프레임에 일어서고 뛴다(LyraGameplayAbility_Jump.cpp:55). 앉지 않았으면 아무 일도 없다
			Target->Jump();
		}
		else Target->StopJumping();
	}
	if (S.bMove) Target->AddMovementInput(FRotator(0.f, BodyYaw + MoveAngle, 0.f).Vector(), 1.f);
	if (Script == EMOBACScript::Events || Script == EMOBACScript::Ads || Script == EMOBACScript::AdsLong || Script == EMOBACScript::Crouch || Script == EMOBACScript::Melee) RunEvents(DeltaTime);
}

// 사건 경로 — 조각 안의 사건 시각이 이 프레임 [SegmentTime, SegmentTime + DeltaTime) 에 들면 일으킨다. 대기(DeltaTime 0)에는 안 일으킨다.
// 봇 조종기는 입력 처리를 부르지 않아(ALyraPlayerController::PostProcessInput 만 부른다) 여기서 상대의 능력 입력을 직접 처리한다
void UMOBAnimCompareSubsystem::RunEvents(float DeltaTime)
{
	FrameEvents.Reset();
	if (FoeFireReleaseLeft >= 0.f)
	{
		FoeFireReleaseLeft -= DeltaTime;
		if (FoeFireReleaseLeft <= 0.f)
		{
			SetFoeFire(false);
			FoeFireReleaseLeft = -1.f;
		}
	}
	if (bFoeMeleeReleasePending)
	{
		bFoeMeleeReleasePending = false;
		if (ULyraAbilitySystemComponent* A = Cast<ULyraAbilitySystemComponent>(FindASC(Foe.Get()))) A->AbilityInputTagReleased(FGameplayTag::RequestGameplayTag(TEXT("InputTag.Ability.Melee")));
		else ++EvNoAsc;
	}
	const FScriptSegment& S = Seg(Script, Segment);
	const auto Due = [&](EACEvent Ev, float At) { return Ev != EACEvent::None && DeltaTime > 0.f && At + EventDelay >= SegmentTime && At + EventDelay < SegmentTime + DeltaTime; };
	const auto Do = [&](EACEvent Ev)
	{
		switch (Ev)
		{
		case EACEvent::Fire:
			SetFoeFire(true);
			FoeFireReleaseLeft = FIRE_TAP_SECONDS;
			++EvFire;
			FrameEvents += TEXT("fire;");
			break;
		case EACEvent::Hold:
			SetFoeFire(true);
			FoeFireReleaseLeft = -1.f;
			++EvHold;
			FrameEvents += TEXT("hold;");
			break;
		case EACEvent::Release:
			SetFoeFire(false);
			FoeFireReleaseLeft = -1.f;
			++EvRelease;
			FrameEvents += TEXT("release;");
			break;
		case EACEvent::Reload:
			PulseFoeReload();
			++EvReload;
			FrameEvents += TEXT("reload;");
			break;
		case EACEvent::Swap:
		case EACEvent::Back:
		{
			const int32 To = Ev == EACEvent::Back ? WeaponIndex : (WeaponIndex == 0 ? 1 : 0);	// 권총이면 소총으로, 아니면 권총으로
			if (EquipFoeWeapon(To)) ++EvSwap;
			else ++EvSwapFail;
			FrameEvents += FString::Printf(TEXT("swap:%s;"), WEAPON_KEYS[To]);
			break;
		}
		case EACEvent::Spawn:
			if (ActivateFoeSpawnEffect()) ++EvSpawn;
			else ++EvSpawnFail;
			FrameEvents += TEXT("spawn;");
			break;
		case EACEvent::AdsOn:
			bFoeAdsHeld = true;
			++EvAdsOn;
			FrameEvents += TEXT("ads_on;");
			break;
		case EACEvent::AdsOff:
			SetFoeAdsReleased();
			++EvAdsOff;
			FrameEvents += TEXT("ads_off;");
			break;
		case EACEvent::CrouchOn:
		case EACEvent::CrouchOff:
			SetFoeCrouch(Ev == EACEvent::CrouchOn);
			FrameEvents += Ev == EACEvent::CrouchOn ? TEXT("crouch_on;") : TEXT("crouch_off;");
			break;
		case EACEvent::Melee:
			if (ULyraAbilitySystemComponent* A = Cast<ULyraAbilitySystemComponent>(FindASC(Foe.Get())))
			{
				A->AbilityInputTagPressed(FGameplayTag::RequestGameplayTag(TEXT("InputTag.Ability.Melee")));
				bFoeMeleeReleasePending = true;
				++EvMelee;
			}
			else ++EvNoAsc;
			FrameEvents += TEXT("melee;");
			break;
		case EACEvent::HitBack:
		case EACEvent::HitFront:
			if (PlayFoeHitReact(Ev == EACEvent::HitBack)) ++(Ev == EACEvent::HitBack ? EvHitBack : EvHitFront);
			else ++EvHitFail;
			FrameEvents += Ev == EACEvent::HitBack ? TEXT("hit_back;") : TEXT("hit_front;");
			break;
		case EACEvent::None:
			break;
		}
	};
	if (Due(S.Ev0, S.At0)) Do(S.Ev0);
	if (Due(S.Ev1, S.At1)) Do(S.Ev1);
	if (ULyraAbilitySystemComponent* A = Cast<ULyraAbilitySystemComponent>(FindASC(Foe.Get())))
	{
		// 조준은 누르는 동안 매 프레임 「눌림」 — IA_ADS 는 트리거가 없어 눌린 동안 매 프레임 Triggered, Lyra 는 그때마다 Pressed 를 보낸다(원본 기준)
		if (bFoeAdsHeld) A->AbilityInputTagPressed(FGameplayTag::RequestGameplayTag(TEXT("InputTag.Weapon.ADS")));
		A->ProcessAbilityInput(DeltaTime, false);
	}
	else ++EvNoAsc;
}

void UMOBAnimCompareSubsystem::SetFoeAdsReleased()
{
	const bool bWasHeld = bFoeAdsHeld;
	bFoeAdsHeld = false;
	if (!bWasHeld) return;
	if (ULyraAbilitySystemComponent* A = Cast<ULyraAbilitySystemComponent>(FindASC(Foe.Get()))) A->AbilityInputTagReleased(FGameplayTag::RequestGameplayTag(TEXT("InputTag.Weapon.ADS")));
	else ++EvNoAsc;
}

// 앉기 단추 — 사람 입력이 부르는 ALyraCharacter::ToggleCrouch 그대로(IA_Crouch 는 누른 프레임에 한 번, content-add-progress.md 0-1 정정).
// 지금 상태 판정도 ToggleCrouch 와 같게(앉았거나 앉으려는 중). 누른 뒤엔 「앉으려는가」만 본다 — 일어서기는 다음 이동 틱에 bIsCrouched 가 바뀐다
void UMOBAnimCompareSubsystem::SetFoeCrouch(bool bWant)
{
	ALyraCharacter* C = Cast<ALyraCharacter>(Foe.Get());
	const UCharacterMovementComponent* M = C ? C->GetCharacterMovement() : nullptr;
	if (!M)
	{
		++EvCrouchFail;
		return;
	}
	if ((C->bIsCrouched || M->bWantsToCrouch) == bWant)
	{
		++EvCrouchSkip;
		return;
	}
	C->ToggleCrouch();
	if (M->bWantsToCrouch != bWant) ++EvCrouchFail;	// 공중에서 누르면 Lyra 는 앉지 않는다
	else if (bWant) ++EvCrouchOn;
	else ++EvCrouchOff;
}

void UMOBAnimCompareSubsystem::SetFoeFire(bool bHeld)
{
	ULyraAbilitySystemComponent* A = Cast<ULyraAbilitySystemComponent>(FindASC(Foe.Get()));
	if (!A)
	{
		++EvNoAsc;
		return;
	}
	const FGameplayTag Fire = FGameplayTag::RequestGameplayTag(TEXT("InputTag.Weapon.Fire"));
	const FGameplayTag FireAuto = FGameplayTag::RequestGameplayTag(TEXT("InputTag.Weapon.FireAuto"));
	if (bFoeFireHeld)	// 당긴 채 다시 당기면 새 누름이 되도록 먼저 놓는다(무기 기록 도구 UMOBWeaponRecordSubsystem::SetFire 와 같은 태그)
	{
		A->AbilityInputTagReleased(Fire);
		A->AbilityInputTagReleased(FireAuto);
		bFoeFireHeld = false;
	}
	if (!bHeld) return;
	A->AbilityInputTagPressed(Fire);
	A->AbilityInputTagPressed(FireAuto);
	bFoeFireHeld = true;
}

void UMOBAnimCompareSubsystem::PulseFoeReload()
{
	if (ULyraAbilitySystemComponent* A = Cast<ULyraAbilitySystemComponent>(FindASC(Foe.Get())))
	{
		const FGameplayTag Reload = FGameplayTag::RequestGameplayTag(TEXT("InputTag.Weapon.Reload"));
		A->AbilityInputTagPressed(Reload);
		A->AbilityInputTagReleased(Reload);
	}
	else ++EvNoAsc;
}

// 빠른 칸을 바꾼다 — 지금 장비를 빼고 그 칸 아이템의 장비를 EquipItem 으로 넣는다(LyraQuickBarComponent::SetActiveSlotIndex).
// 실제 판의 상대는 장비 칸에 바로 넣지만(MOBridgeSubsystem::SyncRemoteWeapon) 둘 다 EquipItem 을 거쳐 꺼내기 동작이 같은 자리에서 튼다. 칸으로 하는 까닭 = 탄약(Prepare 주석)
bool UMOBAnimCompareSubsystem::EquipFoeWeapon(int32 Index)
{
	ACharacter* Target = Foe.Get();
	AController* Ctrl = Target ? Target->GetController() : nullptr;
	ULyraQuickBarComponent* QB = Ctrl ? Ctrl->FindComponentByClass<ULyraQuickBarComponent>() : nullptr;
	if (!QB) return false;
	if (bFoeFireHeld) SetFoeFire(false);
	QB->SetActiveSlotIndex(Index);
	const bool bOk = QB->GetActiveSlotIndex() == Index && QB->GetActiveSlotItem() != nullptr;
	if (bOk) HeldWeapon = Index;
	return bOk;
}

// 맞은 쪽 반응 — 피해 연출 GCNL_Character_DamageTaken 의 MontageHit 이 부르는 것과 같은 ACharacter::PlayAnimMontage(몽타주, 배율 1)(원본 기준, 그래프)
bool UMOBAnimCompareSubsystem::PlayFoeHitReact(bool bBack)
{
	ACharacter* Target = Foe.Get();
	UAnimMontage* Montage = LoadObject<UAnimMontage>(nullptr, bBack ? HIT_BACK_PATH : HIT_FRONT_PATH);
	if (!Target || !Montage)
	{
		UE_LOG(LogMOBAComp, Warning, TEXT("MOB_ACOMP hit_react target=%d montage=%d"), Target ? 1 : 0, Montage ? 1 : 0);
		return false;
	}
	return Target->PlayAnimMontage(Montage, 1.f) > 0.f;
}

// 부활 효과 능력을 다시 켠다 — 이미 켜져 있으면 끄고 켠다
bool UMOBAnimCompareSubsystem::ActivateFoeSpawnEffect()
{
	UAbilitySystemComponent* A = FindASC(Foe.Get());
	UClass* C = LoadClass<UGameplayAbility>(nullptr, SPAWN_EFFECT_PATH);
	FGameplayAbilitySpec* Spec = (A && C) ? A->FindAbilitySpecFromClass(C) : nullptr;
	if (!Spec)
	{
		UE_LOG(LogMOBAComp, Warning, TEXT("MOB_ACOMP spawn_effect asc=%d class=%d spec=0"), A ? 1 : 0, C ? 1 : 0);
		return false;
	}
	if (Spec->IsActive()) A->CancelAbilityHandle(Spec->Handle);
	return A->TryActivateAbility(Spec->Handle);
}

// 처음 본 몽타주 자산의 값을 한 줄씩 남긴다 — 서버 쪽 몽타주 표(클립 파일 버전 2)의 입력. 같은 프레임에 살아 있는 몽타주 수의 최대도 센다
void UMOBAnimCompareSubsystem::NoteMontageAssets()
{
	const ACharacter* Target = Foe.Get();
	const USkeletalMeshComponent* Mesh = Target ? Target->GetMesh() : nullptr;
	const UAnimInstance* Main = Mesh ? Mesh->GetAnimInstance() : nullptr;
	if (!Main) return;
	MaxLiveMontages = FMath::Max(MaxLiveMontages, Main->MontageInstances.Num());
	if (MontageCsv.IsEmpty())
		MontageCsv = TEXT("name,group,playLength,rateScale,blendIn,blendInOption,blendOut,blendOutOption,blendOutTrigger,autoBlendOut,blendModeIn,blendModeOut,profileIn,profileOut,slots,notifies,curves,firstFrame,firstSegment\n");
	for (const FAnimMontageInstance* I : Main->MontageInstances)
	{
		const UAnimMontage* M = I ? I->Montage.Get() : nullptr;
		if (!M || SeenMontages.Contains(M->GetFName())) continue;
		SeenMontages.Add(M->GetFName());
		FString Slots;
		for (const FSlotAnimationTrack& T : M->SlotAnimTracks)
		{
			Slots += T.SlotName.ToString() + TEXT(":");
			for (const FAnimSegment& G : T.AnimTrack.AnimSegments)
				Slots += FString::Printf(TEXT("%s[%.4f %.4f-%.4f r%.3f x%d]+"), *GetNameSafe(G.GetAnimReference().Get()), G.StartPos, G.AnimStartTime, G.AnimEndTime, G.AnimPlayRate, G.LoopingCount);
			Slots += TEXT("|");
		}
		FString Notifies;
		for (const FAnimNotifyEvent& N : M->Notifies)
			Notifies += FString::Printf(TEXT("%s@%.4f(%s);"), *N.NotifyName.ToString(), N.GetTriggerTime(), *GetNameSafe(N.Notify ? N.Notify->GetClass() : (N.NotifyStateClass ? N.NotifyStateClass->GetClass() : nullptr)));
		FString Curves;
		for (const FFloatCurve& C : M->GetCurveData().FloatCurves) Curves += C.GetName().ToString() + TEXT(";");
		MontageCsv += FString::Printf(TEXT("%s,%s,%.5f,%.4f,%.4f,%d,%.4f,%d,%.4f,%d,%d,%d,%s,%s,%s,%s,%s,%d,%s\n"),
			*M->GetName(), *M->GetGroupName().ToString(), M->GetPlayLength(), M->RateScale,
			M->BlendIn.GetBlendTime(), (int32)M->BlendIn.GetBlendOption(), M->BlendOut.GetBlendTime(), (int32)M->BlendOut.GetBlendOption(),
			M->BlendOutTriggerTime, M->bEnableAutoBlendOut ? 1 : 0, (int32)M->BlendModeIn, (int32)M->BlendModeOut,
			*GetNameSafe(M->BlendProfileIn.Get()), *GetNameSafe(M->BlendProfileOut.Get()),
			*Slots.Replace(TEXT(","), TEXT(" ")), *Notifies.Replace(TEXT(","), TEXT(" ")), *Curves.Replace(TEXT(","), TEXT(" ")), Frame, Seg(Script, Segment).Name);
	}
}

// 골격의 뼈별 비중 표(블렌드 프로파일) 전부 — 윗몸 몽타주가 어느 뼈에 얼마나 섞이는지의 입력(9-1). 표마다 뼈 · 비중 한 줄
void UMOBAnimCompareSubsystem::WriteBlendProfiles()
{
	const ACharacter* Target = Foe.Get();
	const USkeletalMeshComponent* Mesh = Target ? Target->GetMesh() : nullptr;
	const USkeletalMesh* SkMesh = Mesh ? Mesh->GetSkeletalMeshAsset() : nullptr;
	USkeleton* Skel = SkMesh ? const_cast<USkeleton*>(SkMesh->GetSkeleton()) : nullptr;	// 뼈별 비중 표를 이름으로 찾는 함수가 const 가 아니다(읽기만 한다)
	if (!Skel)
	{
		UE_LOG(LogMOBAComp, Warning, TEXT("MOB_ACOMP blend_profiles skeleton=0"));
		return;
	}
	BlendProfileCsv = TEXT("profile,mode,bone,scale\n");
	int32 Entries = 0;
	for (const UBlendProfile* P : Skel->BlendProfiles)
	{
		if (!P) continue;
		for (const FBlendProfileBoneEntry& E : P->ProfileEntries)
		{
			BlendProfileCsv += FString::Printf(TEXT("%s,%d,%s,%.6f\n"), *P->GetName(), (int32)P->Mode, *E.BoneReference.BoneName.ToString(), E.BlendScale);
			++Entries;
		}
	}
	UE_LOG(LogMOBAComp, Log, TEXT("MOB_ACOMP blend_profiles skeleton=%s profiles=%d entries=%d upperLower=%d"),
		*Skel->GetName(), Skel->BlendProfiles.Num(), Entries, Skel->GetBlendProfile(FName(UPPER_LOWER_MASK)) ? 1 : 0);
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
	if (VideoPass >= 0 && Pass != VideoPass) return;	// -MOBACVideoPass — 디스크가 모자랄 때 한 판만 찍는다
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
		UE_LOG(LogMOBAComp, Log, TEXT("MOB_ACOMP probe vars=%d machines=%d chars=%d saved=%d rebinds=%d blankRows=%d"), ProbeVars.Num(), ProbeMachines.Num(), ProbeCsv.Len(), bProbeSaved ? 1 : 0, ProbeRebinds, ProbeBlankRows);
		const FString MontagePath = FPaths::ProjectSavedDir() / TEXT("MOB") / FString::Printf(TEXT("animcmp_%s_montages.csv"), *Tag);
		const bool bMontSaved = !MontageCsv.IsEmpty() && FFileHelper::SaveStringToFile(MontageCsv, *MontagePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		const FString ProfilePath = FPaths::ProjectSavedDir() / TEXT("MOB") / FString::Printf(TEXT("animcmp_%s_blendprofiles.csv"), *Tag);
		const bool bProfSaved = !BlendProfileCsv.IsEmpty() && FFileHelper::SaveStringToFile(BlendProfileCsv, *ProfilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		UE_LOG(LogMOBAComp, Log, TEXT("MOB_ACOMP montages seen=%d maxLive=%d saved=%d profilesSaved=%d"), SeenMontages.Num(), MaxLiveMontages, bMontSaved ? 1 : 0, bProfSaved ? 1 : 0);
	}
	if (Script == EMOBACScript::Events || Script == EMOBACScript::Ads || Script == EMOBACScript::AdsLong || Script == EMOBACScript::Crouch || Script == EMOBACScript::Melee)
	{
		// 종류마다 센 값 — 경로 표에 적힌 수(판 둘 × 조각의 사건 수)와 대조한다
		UE_LOG(LogMOBAComp, Log, TEXT("MOB_ACOMP events weapon=%s fire=%d hold=%d release=%d reload=%d swap=%d swapFail=%d spawn=%d spawnFail=%d adsOn=%d adsOff=%d noAsc=%d"),
			WEAPON_KEYS[WeaponIndex], EvFire, EvHold, EvRelease, EvReload, EvSwap, EvSwapFail, EvSpawn, EvSpawnFail, EvAdsOn, EvAdsOff, EvNoAsc);
		if (Script == EMOBACScript::Melee)
		{
			UE_LOG(LogMOBAComp, Log, TEXT("MOB_ACOMP meleeEvents weapon=%s melee=%d hitBack=%d hitFront=%d hitFail=%d crouchOn=%d crouchOff=%d"), WEAPON_KEYS[WeaponIndex], EvMelee, EvHitBack, EvHitFront, EvHitFail, EvCrouchOn, EvCrouchOff);
		}
		if (Script == EMOBACScript::Crouch)
		{
			UE_LOG(LogMOBAComp, Log, TEXT("MOB_ACOMP crouchEvents weapon=%s on=%d off=%d skip=%d fail=%d"), WEAPON_KEYS[WeaponIndex], EvCrouchOn, EvCrouchOff, EvCrouchSkip, EvCrouchFail);
		}
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
	// 상대가 죽어 사라지면(바닥 밖으로 떨어짐 등) 여기서 끝낸다 — 그냥 두면 BeginPass 가 조각 번호를 안 되돌려 경로 표 밖을 읽는다(11-12 사건 경로 첫 판에서 그렇게 죽었다).
	// 되살아난 봇은 머리가 켜져 있어 스스로 쏘므로 그 뒤 기록은 쓸 수 없다
	if ((Stage == EStage::Settle || Stage == EStage::Run) && !Foe.IsValid())
	{
		UE_LOG(LogMOBAComp, Error, TEXT("MOB_ACOMP_FAIL foe_lost pass=%d segment=%d frame=%d"), Pass, Segment, Frame);
		if (Grabber) Grabber->StopCapturingFrames();
		Stage = EStage::Stopping;
	}
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
