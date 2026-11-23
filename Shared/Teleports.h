#pragma once

#include "Movement.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// 순간이동(teleport-plan) — L_Expanse 보라 순간이동 B_Teleport 자식 6개(이름표 B_Launcher_Teleport_Local 4 · B_DoorPortal 2). 값은 리포 밖 파일(서버 --teleports=,
// Tools/LyraPilot/make_teleports.py 가 Lyra 실행값 덤프에서 만든다 — Epic 맵 값이라 리포에 넣지 않는다, 점프대 표와 같은 판단). 옮기는 규칙은 B_Teleport 그래프 그대로다
// (make_teleports.py GRAPH_GUARDS 가 그래프 글에서 같은 마디를 확인해야 파일을 쓴다 — 칸 겹침 시작 → 짝 장치의 Exit Direction 자리 · 그 앞방향으로 Teleport →
// LaunchOnTeleport 면 앞방향 × 들어간 장치의 LaunchMultiplier 로 LaunchCharacter(XY · Z 덮음) → bForceDirection 이면 조종 방향도 그쪽으로).
// 서버 · 내 몸 예측(봇 · 다리) · MoveCheck 가 같은 함수를 부른다 — 부르는 자리는 점프대와 같은 TickWithLaunchers(JumpPads.h) 하나다.
// 수류탄이 칸에 들어가 옮겨지는 그래프 갈래는 범위 밖이다(진행 파일 미결 ①).
namespace mo
{
	// 겹침 비트가 한 바이트(MoveState::teleportMask)
	constexpr size_t kMaxTeleports = 8;
	// 이름 최대 글자 수 — 점프대 이름과 같은 63자(와이어에 실을 때 0 으로 끝나는 64바이트 칸). 넘는 줄은 읽기에서 틀린 줄로 본다
	constexpr size_t kMaxTeleportName = 63;

	struct Teleporter
	{
		std::string name;		// Lyra 액터 이름 — 다리가 이 이름으로 맵의 장치를 찾는다
		int target = -1;		// 짝 장치 번호(이 표 차례) — 옮겨 갈 곳
		Vec3 boxCenter;			// 겹침 칸(Box 컴포넌트) 월드 중심 · 월드 축(단위 벡터) · 배율 반영 반너비. 칸은 서 있어야 한다(z 축 = 월드 위 — 읽기에서 거른다)
		Vec3 boxAxisX;
		Vec3 boxAxisY;
		Vec3 boxExtent;
		Vec3 root;				// 이 장치 뿌리(DefaultSceneRoot1) 월드 자리 · 앞방향 — 「들어간 자리 그대로」 몫이 이 면에 투영한다
		Vec3 rootForward;
		Vec3 exit;				// 짝 장치의 Exit Direction 월드 자리 · 앞방향(단위 벡터) — 옮긴 자리 · 몸 방향 · 뒤따르는 발사 방향
		Vec3 exitForward;
		bool keepRelative = false;		// KeepReleativeEntryUponExit(문 포털) — 들어간 자리 차이를 나갈 자리에 더한다
		bool launchOnTeleport = false;
		double launchMultiplier = 0;	// 들어간 장치 것(짝 것이 아니다 — t-2 기록 37번이 가른다)
		bool forceDirection = false;	// bForceDirection — 조종 방향(카메라)도 나갈 방향으로
		double offsetZMax = 0;			// 「들어간 자리 그대로」 위아래 몫 상한 = TeleporterHeight − 100(하한 0)
	};

	struct TeleportTable
	{
		std::vector<Teleporter> items;
		double meshBaseZ = 0;			// TELEPORT_PAWN — 선 몸 메시 자리 = 캡슐 중심 + meshBaseZ(Lyra −88)
		double standHalfHeight = 0;		// 그 값을 잰 선 캡슐 반높이 — 부르는 쪽이 이동 설정과 같은지 본다
	};

	enum class ETeleportsLoad
	{
		Ok,
		BadLine,		// 칸 수 · 숫자 · 범위가 틀린 줄(칸이 기울었음 · 차례 · 짝 번호 포함)
		NoTeleports,	// TELEPORT 줄이 하나도 없음
		TooMany,		// kMaxTeleports 넘음
		NoPawn,			// TELEPORT_PAWN 줄이 없거나 둘 이상
	};

	// 줄 = "TELEPORT_PAWN meshBaseZ standHalfHeight" 한 번과
	// "TELEPORT 차례 이름 짝 칸중심3 칸x축3 칸y축3 칸z축3 칸반너비3 뿌리3 뿌리앞3 나갈자리3 나갈앞3 keepRelative launchOnTeleport launchMultiplier forceDirection offsetZMax"
	// (# 뒤는 주석, 빈 줄 무시). 차례는 0 부터 줄 순서, 짝은 표 안의 다른 장치. 틀린 줄이 하나라도 있으면 아무것도 넘기지 않는다. outBadLine = 틀린 줄 번호(1부터)
	ETeleportsLoad ParseTeleports(const char* text, size_t size, TeleportTable& out, size_t& outBadLine);
	const char* TeleportsLoadName(ETeleportsLoad result);

	struct TeleportSet
	{
		const Teleporter* items = nullptr;
		size_t count = 0;
		double meshBaseZ = 0;
	};

	// 몸 캡슐(축이 세로)과 서 있는 상자 칸이 겹치나 — 축 선분과 상자 사이 거리 < 캡슐 반지름
	bool TeleportOverlaps(const Teleporter& teleporter, const Vec3& capsuleCenter, double capsuleRadius, double capsuleHalfHeight);
	uint8_t TeleportMask(const TeleportSet& teleports, const Vec3& capsuleCenter, double capsuleRadius, double capsuleHalfHeight);

	// 옮길 자리(캡슐 중심) — meshZ = 캡슐 중심에서 메시까지 높이(선 몸 meshBaseZ, 앉은 몸은 + CrouchMeshLiftZ — 메시 세상 자리는 앉아도 같다)
	Vec3 TeleportDestination(const Teleporter& teleporter, const Vec3& capsuleCenter, double meshZ);
	// 나갈 방향 요(도) — RotationFromXVector(나갈 앞방향).Yaw. 옮긴 뒤 몸 방향 · bForceDirection 이면 조종 요
	double TeleportExitYaw(const Teleporter& teleporter);
	// 뒤따르는 발사(LaunchOnTeleport 일 때) — 셋 다 덮으므로 지금 속도를 안 더한다
	Vec3 TeleportLaunch(const Teleporter& teleporter);

	struct TeleportStep
	{
		int begins = 0;			// 이번 이동 끝에 겹침이 새로 시작된 칸 수
		int device = -1;		// 들어가 옮겨진 장치 번호(여럿이면 표 차례 첫째). -1 = 안 옮김
		double yaw = 0;			// 옮겼으면 나갈 방향 요
		bool forceYaw = false;	// 조종 요도 yaw 로(bForceDirection) — 클라가 제 시점을 돌린다
		bool launch = false;	// 뒤따르는 발사를 pendingLaunch 에 넣었다
	};

	// 이동 끝 겹침 시작을 보고 옮긴다 — TickWithLaunchers 가 이동 직후 점프대 겹침보다 먼저 부른다(그래프의 Teleport 가 옮긴 자리에서 겹침을 다시 세므로).
	// 옮기면 자리 · 비트(옮긴 자리로 다시 셈) · 이동 모드(CMoveSim::OnTeleported) · 뒤따르는 발사가 바뀐다. 옮긴 자리가 다른 칸 안이면 Lyra 는 잇달아 옮길 것으로
	// 보지만(그래프 갈래 넷 = 발사 · bForceDirection · 소리 · 지을 때 ReceiverOnly — DoOnce · Gate · Delay 없고 ReceivingPawn 은 쓰기만 한다) 여기는 한 번만 옮긴다 — L_Expanse 나갈 자리 6곳은 다 칸 밖이라 해당 없음(MoveCheck TELE_TABLE 이 센다).
	// 속도는 그대로(기록 53번 전부). 발사는 부른 쪽이 점프대 셈을 마친 뒤 넣어야 한다 — 그래프에서 LaunchCharacter 가 Teleport 다음이라 점프대 발사를 덮는다(아래 launch).
	// 모자란 점 — 점프대와 같이 이동 끝 자리만 본다(한 걸음 안에 칸을 지나치면 Lyra 는 옮기지만 여기는 안 옮김 — 가장 얇은 칸 깊이 64cm + 캡슐 지름 70cm 라
	// 1/60초 걸음이 2,500cm/s 발사 속도여도 42cm 라 못 지나친다)
	TeleportStep ApplyTeleports(CMoveSim& sim, MoveState& state, const TeleportSet& teleports, double capsuleRadius, Vec3& outLaunch);

	// 밖에서 자리를 정해 준 뒤(부활 · 시험 놓기 · 보정) — 비트를 지금 자리로 다시 센다. 놓인 자리가 칸 안이어도 옮기지 않는다(점프대 ResetLaunchers 와 같은 판단)
	void ResetTeleports(MoveState& state, const TeleportSet& teleports, double capsuleRadius, double capsuleHalfHeight);

	// 보정 — 받은 자리가 어느 장치의 「옮긴 자리」인가(상태 패킷 깃발 STATE_LAUNCH_PENDING 이 순간이동 발사 몫인지 가린다). 돌림 = 들어간 장치 번호, -1 = 아님.
	// 나갈 자리가 하나로 정해지는 장치는 그 점과 tolerance 안. 「들어간 자리 그대로」 장치는 나갈 면(들어간 장치 뿌리 앞방향이 법선) 위 tolerance 안 ·
	// 위아래 몫이 0 ~ offsetZMax · 옆 몫이 칸 반대각 + 캡슐 반지름 + 칸 중심 ↔ 뿌리 거리 안. 받은 자리는 단정밀도라 tolerance 는 그 반올림보다 넉넉해야 한다
	int MatchTeleportExit(const TeleportSet& teleports, const Vec3& location, double capsuleRadius, double tolerance);
}
