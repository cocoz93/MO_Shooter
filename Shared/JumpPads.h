#pragma once

#include "GameProtocol.h"
#include "MapCollision.h"
#include "Movement.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// 점프대(jumppad-plan) — L_Expanse 노란 점프대 B_Launcher_Up · B_Launcher_Push. 값은 리포 밖 파일(서버 --launchers=, Tools/LyraPilot/make_launchers.py 가
// Lyra 실행값 덤프에서 만든다 — Epic 맵 값이라 리포에 넣지 않는다, 받침 pads_expanse.txt 와 같은 판단). 발사 규칙의 상수는 청사진 그래프 마디 값이라 여기 박는다
// (make_launchers.py GRAPH_GUARDS 가 그래프 글에서 같은 값을 확인해야 파일을 쓴다).
// 서버 · 내 몸 예측(봇 · 다리) · MoveCheck 가 같은 함수를 부른다 — 쏘는 틱에 예측이 서버와 같아야 화면이 안 튄다.
// 순간이동(B_Teleport)은 이 범위 밖이다(D157 — 사용자 말 「점프대도」).
namespace mo
{
	// 겹침 비트가 한 바이트(MoveState::launcherMask)
	constexpr size_t kMaxLaunchers = 8;
	// 이름 최대 글자 수 — 와이어 칸(S2C_LAUNCHER)이 0 으로 끝나는 64바이트라서. 넘는 줄은 읽기에서 틀린 줄로 본다
	constexpr size_t kMaxLauncherName = wire::kLauncherNameBytes - 1;

	enum class ELauncherKind : uint8_t { Up, Push };

	struct Launcher
	{
		ELauncherKind kind = ELauncherKind::Up;
		std::string name;			// Lyra 액터 이름 — 다리가 이 이름으로 맵의 점프대를 찾는다
		Vec3 sphereCenter;			// 겹침 칸(Sphere 컴포넌트) 월드 중심 · 배율 반영 반지름
		double sphereRadius = 0;
		double upOverride = 0;		// 위로: 발사 수직 속도 · 밀기: 목표를 안 쓸 때 수직 몫(× Multiplier)
		double multiplier = 0;		// 밀기 — 목표를 안 쓸 때 배율
		bool useTarget = false;		// 밀기 — Target Method(목표 거리로 셈)
		Vec3 target;				// 밀기 — 짓기 그래프가 자른 뒤 값(Y 는 늘 0)
		double minVerticalVelocity = 0;	// MinimumVerticalVelocityAllowed — noFallEntry 일 때만 쓴다
		bool noFallEntry = false;	// Do Not Activate on Fall Entry — 지난 이동 끝 수직 속도가 위 값보다 작으면 안 쏜다
	};

	enum class ELaunchersLoad
	{
		Ok,
		BadLine,		// 칸 수 · 숫자 · 범위가 틀린 줄
		NoLaunchers,	// LAUNCHER 줄이 하나도 없음
		TooMany,		// kMaxLaunchers 넘음
	};

	// 줄 = "LAUNCHER 종류 이름 구x 구y 구z 구반지름 upOverride multiplier useTarget 목표x 목표y 목표z 최소수직속도 noFallEntry" (# 뒤는 주석, 빈 줄 무시).
	// 종류 = up | push. 틀린 줄이 하나라도 있으면 아무것도 넘기지 않는다. outBadLine = 틀린 줄 번호(1부터, Ok 면 0)
	ELaunchersLoad ParseLaunchers(const char* text, size_t size, std::vector<Launcher>& out, size_t& outBadLine);
	const char* LaunchersLoadName(ELaunchersLoad result);
	const char* LauncherKindName(ELauncherKind kind);

	// 몸 캡슐(축이 세로) 과 점프대 구 칸이 겹치나 — 축 선분과 구 중심 거리 < 두 반지름 합
	bool LauncherOverlaps(const Launcher& launcher, const Vec3& capsuleCenter, double capsuleRadius, double capsuleHalfHeight);
	uint8_t LauncherMask(const Launcher* launchers, size_t count, const Vec3& capsuleCenter, double capsuleRadius, double capsuleHalfHeight);

	// 점프대 그래프 Overlap 한 번 — 쏘면 참이고 outLaunch = LaunchCharacter 가 PendingLaunchVelocity 에 넣는 값(덮지 않는 축은 지금 속도를 더한 뒤).
	// velocity = 겹침 사건 때 몸 속도(이동 묶음이 닫힐 때 = 이동 끝 속도) · yawDegrees = 몸 방향(앞방향 단위 벡터를 쓴다) · lastUpdateVelocity = 「떨어지며 들어옴」 검사
	bool LauncherLaunch(const Launcher& launcher, const Vec3& velocity, const Vec3& lastUpdateVelocity, double yawDegrees, Vec3& outLaunch);

	struct LauncherSet
	{
		const Launcher* items = nullptr;
		size_t count = 0;
	};

	struct LauncherTickResult
	{
		bool launchedThisTick = false;	// 이번 이동 머리에서 지난 걸음이 남긴 발사를 넣었다
		int begins = 0;					// 이번 이동 끝에 겹침이 새로 시작된 점프대 수
		int fired = -1;					// 그중 쏜 점프대 번호(여럿이면 마지막 — 발사는 덮어쓴다). -1 = 안 쏨
		int refused = 0;				// 겹침은 시작했는데 그래프가 안 쏜 수(떨어지며 들어옴)
	};

	// 점프대와 함께 이동 한 걸음 — 지난 걸음 끝 겹침 시작이 남긴 발사를 이번 이동 머리에 넣고(HandlePendingLaunch), 이동 뒤 겹침 시작을 본다
	// (겹침 사건은 이동 묶음이 닫힐 때 난다 — CharacterMovementComponent.cpp:2225 FScopedCapsuleMovementUpdate). 발사는 그 다음 걸음에 먹는다(j-2 기록 51번 전부).
	// 점프대가 없으면(count 0) sim.Tick 한 번과 같고 MoveState 의 점프대 칸도 안 바뀐다.
	// 모자란 점 — 이동 끝 자리만 본다. 한 걸음 안에 구 칸을 들어갔다 나오면 Lyra(쓸기 겹침)는 쏘지만 여기는 안 쏜다(j-2 기록엔 없음)
	LauncherTickResult TickWithLaunchers(CMoveSim& sim, MoveState& state, const Vec3& input, float deltaTime, double yawDegrees, const LauncherSet& launchers, double capsuleRadius);

	// 밖에서 자리를 정해 준 뒤(부활 · 시험 놓기 · 보정) — 겹침 비트를 지금 자리로 다시 세고 남은 발사를 지운다. 놓인 자리가 점프대 안이어도 안 쏜다
	// (Lyra 는 쓸지 않고 놓을 때도 이동 끝에 겹침을 갱신해 시작을 낸다 — PrimitiveComponent.cpp:3512-3534. 그래도 L_Expanse 부활 자리 20곳은
	// 가장 가까운 것도 구 칸과 717cm 떨어져 해당이 없고, 점프대 안에 놓는 것은 시험 놓기뿐이다 — 계획 「목표 구조」)
	void ResetLaunchers(MoveState& state, const LauncherSet& launchers, double capsuleRadius, double capsuleHalfHeight);

	// 받은 상태로 점프대 칸을 다시 세운다(예측 보정 — 서버 자리 · 속도로 MoveState 를 덮은 뒤). 비트는 그 자리로 세고, launchPending(STATE_LAUNCH_PENDING)이면
	// 그 자리에서 겹친 점프대(여럿이면 표 차례 마지막)에 상태 속도 · 요를 넣어 발사를 다시 셈한다 — 서버는 이동 끝 속도 · 그 입력 요로 셈했고
	// 상태 패킷의 속도 · 요가 바로 그 값이다(단정밀도로 줄었을 뿐). false = 깃발은 켜졌는데 겹친 점프대가 없거나 그래프가 안 쏘는 값(부른 쪽이 센다). 그때 발사는 지운다
	bool RestoreLaunchers(MoveState& state, const LauncherSet& launchers, double capsuleRadius, double capsuleHalfHeight, bool launchPending, double yawDegrees);

	// 와이어 한 줄(S2C_LAUNCHER) — 쓰기는 이름이 kMaxLauncherName 을 넘거나 차례가 틀리면 false. 읽기는 종류 · 깃발 · 반지름 · 이름 끝 · 차례가 틀리면 false
	bool LauncherToWire(const Launcher& launcher, size_t index, size_t count, wire::LauncherInfo& out);
	bool LauncherFromWire(const wire::LauncherInfo& in, Launcher& out);
}
