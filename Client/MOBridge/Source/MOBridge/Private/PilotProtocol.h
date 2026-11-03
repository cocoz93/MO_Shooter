#pragma once
// ─────────────────────────────────────────────────────────────
//  Lyra 다리 파일럿 — 와이어 형식
//
//  ★ 두 벌이다. 같은 내용이
//      MO_Shooter/Tools/LyraPilot/PilotProtocol.h          (파일럿 서버)
//      LyraStarterGame/Plugins/MOBridge/Source/MOBridge/Private/PilotProtocol.h  (Lyra 쪽)
//    에 있다. 한쪽을 고치면 다른 쪽도 똑같이 고칠 것 — 파일럿이라 공용 폴더를 만들지 않았다.
//
//  머리말 = ServerCore 의 MsgHeader 와 같은 4바이트(size = 머리말 포함 전체, type).
//  리틀 엔디언 · 1바이트 정렬. 좌표 단위는 언리얼 그대로 cm.
// ─────────────────────────────────────────────────────────────
#include <cstdint>

namespace Pilot
{
    enum class Msg : uint16_t
    {
        // 서버 → Lyra
        S2C_SPAWN_REMOTE = 1,   // 원격 캐릭터 하나 띄우기
        S2C_REMOTE_MOVE  = 2,   // 원격 캐릭터 위치(서버 틱마다)
        S2C_DAMAGE       = 3,   // 서버가 정한 피해 — 대상 체력은 서버가 정본
        S2C_WEAPON_TABLE = 4,   // 무기별 피해 표(접속 직후)
        S2C_SCRIPT_END   = 5,   // 각본 끝 — Lyra 는 요약을 찍고 끝낸다

        // Lyra → 서버
        C2S_HELLO        = 100, // 접속 인사 + 내 캐릭터 자리(각본 경로를 그 앞에 깐다)
        C2S_HIT          = 101, // 「누구를 · 어느 무기로」 맞혔다 — 피해는 서버가 정한다
    };

    // 각본 구간 — 1단계 오차를 구간별로 나눠 재려고 같이 보낸다
    enum class MovePhase : uint8_t { Idle = 0, Accel = 1, Cruise = 2, Brake = 3 };

    enum class Weapon : uint8_t { Unknown = 0, Rifle = 1, Pistol = 2, Shotgun = 3, Melee = 4, Grenade = 5 };

    constexpr int32_t LOCAL_PLAYER_ID = 0;   // S2C_DAMAGE 대상 0 = Lyra 쪽 내 캐릭터

#pragma pack(push, 1)
    struct Header
    {
        uint16_t size;   // 머리말 포함 전체
        uint16_t type;   // Msg
    };

    struct SpawnRemote  { Header h; int32_t id; float x, y, z, yaw; };
    // ax, ay = 입력 가속 — 「사람이 지금 키를 누르고 있는 방향 × 최대 가속」. 가속·등속 구간은 누름, 제동·멈춤은 뗌(0).
    //   Lyra 애니는 이 값의 유무로 출발·정지를 가른다(C 방법이 그대로 입력한다)
    struct RemoteMove   { Header h; int32_t id; uint32_t tick; float x, y, z, yaw, vx, vy; uint8_t phase; float ax, ay; };
    struct Damage       { Header h; int32_t targetId; float amount; float hpAfter; uint32_t seq; };
    struct WeaponEntry  { uint8_t weapon; float damage; };
    struct WeaponTable  { Header h; uint8_t count; WeaponEntry entries[8]; };   // 보낼 때는 count 만큼만 잘라 보낸다
    struct ScriptEnd    { Header h; };

    struct Hello        { Header h; float x, y, z, yaw; };
    struct Hit          { Header h; int32_t targetId; uint8_t weapon; uint32_t seq; };
#pragma pack(pop)

    static_assert(sizeof(Header) == 4, "ServerCore MsgHeader 와 같아야 한다");
}
