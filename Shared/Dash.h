#pragma once

#include "Movement.h"

#include <cstdint>

// 대시(컨텐츠 대시) — Lyra GA_Hero_Dash 를 엔진 없이 옮긴 것. 정본 = MyDocs/claude-plans/mo-shooter/dash-plan.md
// - 이 파일 D-1 = 힘 하나(방향 → 힘 벡터 · 힘 설정). 방향 고르기 · 켜짐 · 쿨다운 · 막기는 D-2 에서 더한다
// - 언리얼 안에서도 컴파일돼야 하므로 예외·RTTI 를 쓰지 않는다
namespace mo
{
	// 그래프 변수 · 노드 핀(원본 기준 — dash-plan.md 「Lyra」 표 「힘」 · 「힘이 끝나면」 줄)
	inline constexpr float kDashStrength = 1850.f;			// Strength — 힘 크기(초당 cm). D-0 실측 = 서서 누르면 켠 프레임 속도 1850
	inline constexpr float kDashForceDuration = 0.25f;		// RootMotionDuration — 60fps 에서 15프레임(D-0 실측 힘 프레임 79~93)
	inline constexpr float kDashClampOnFinish = 1000.f;		// ClampVelocityOnFinish — 힘이 끝나면 수평을 이 크기로 · 위를 이 값 이하로 자른다

	// 몽타주 넷 = 방향 넷(SelectDirectionalMontage)
	enum class EDashDir : uint8_t
	{
		Forward,	// 카메라 앞으로 — 위를 보면 위로도 뜬다
		Backward,	// 나머지 셋 = 마지막 이동 입력 쪽
		Left,
		Right,
	};

	// 노드 핀 bEnableGravity=false(위아래도 더함) · VelocityOnFinishMode=ClampVelocity · ClampVelocityOnFinish=1000
	RootForceSettings DashForceSettings();

	// 힘 벡터 = 방향의 단위 벡터 × 1850. 방향 = 앞이면 카메라 앞 × (1, 1, 1.2), 그 밖이면 마지막 이동 입력(월드 축, 길이는 상관없다).
	// 작업이 방향을 정규화한다(엔진 AbilityTask_ApplyRootMotionConstantForce.cpp:40) — 그래서 위를 본 앞 대시는 수평이 준다(D-0 pitch_up20 수평 1695.35).
	// viewPitch · viewYaw = 카메라(= 조종 회전) 도. 방향이 0 이면 0 을 돌려준다 — 능력은 그 전에 취소한다(D-2)
	Vec3 DashForce(EDashDir dir, const Vec3& lastInput, float viewPitch, float viewYaw);
}
