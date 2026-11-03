// ─────────────────────────────────────────────────────────────
//  Lyra 다리 파일럿 서버 — 정해진 각본만 흘리는 서버
//
//  목적은 Lyra 쪽 판정(「서버 결과를 Lyra 에 넣을 수 있나」)이라 게임 로직은 없다.
//  플랜 = MyGit/MyDocs/claude-plans/mo-shooter/lyra-bridge-pilot-plan.md
//
//    LyraPilot move   [--speed=600 --accel=1200 --brake=1400]   1단계: 원격 캐릭터 이동
//    LyraPilot damage                                             2단계: 서버가 체력 깎기
//    LyraPilot weapon [--rifle=N --pistol=N --shotgun=N]          3단계: 무기 수치를 서버가 정하기
//    LyraPilot idle                                               원격 캐릭터만 세워 둠(막기 시험용)
//
//  포트 7790. 접속은 Lyra 하나만 받는다(파일럿).
// ─────────────────────────────────────────────────────────────
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <queue>
#include <chrono>
#include <thread>
#include <map>
#include <atomic>

#include "NetIoModel.h"
#include "MonitorManager.h"
#include <Common/Logger.h>

#include "PilotProtocol.h"

using namespace Pilot;
using Clock = std::chrono::steady_clock;

// ServerCore 가 실행 파일 쪽에 요구하는 종료 신호(IOCPServer.cpp 가 부른다) — 다음 틱에 루프를 끝낸다
std::atomic<bool> g_shutdownRequested{ false };
void SignalProcessShutdown() { g_shutdownRequested = true; }

namespace
{
    constexpr int    PORT       = 7790;
    constexpr double TICK_SEC   = 1.0 / 60.0;
    constexpr int    REMOTE_ID  = 1;
    constexpr float  DEG        = 180.0f / 3.14159265f;

    struct Vec2 { float x = 0, y = 0; };
    Vec2  operator+(Vec2 a, Vec2 b) { return { a.x + b.x, a.y + b.y }; }
    Vec2  operator-(Vec2 a, Vec2 b) { return { a.x - b.x, a.y - b.y }; }
    Vec2  operator*(Vec2 a, float s) { return { a.x * s, a.y * s }; }
    float Len(Vec2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }

    struct Args
    {
        std::string mode = "move";
        // Lyra 실행 중 실측(MOB_READY 줄): 걷기 최고 600 · 최대 가속 1200 · 제동 1400.
        //   C++ 기본 가속 2400(LyraCharacter.cpp:51)은 블루프린트가 1200 으로 덮는다
        float speed = 600.f, accel = 1200.f, brake = 1400.f;
        // 제동 마찰 — Lyra 실측 감속 = 1482 + 2.95 × 속도(10-26 언리얼 기본 이동 실행 75표본) ≈ 제동 1400 + 3 × 속도.
        //   서버가 1400 으로만 감속하면 Lyra 정지 애니와 어긋나 멈출 때마다 정지→출발→정지가 한두 프레임 깜빡였다
        float brakeFriction = 3.f;
        float rifle = 18.f, pistol = 18.f, shotgun = 12.f;
    };

    Args ParseArgs(int argc, char** argv)
    {
        Args a;
        for (int i = 1; i < argc; ++i)
        {
            std::string s = argv[i];
            auto val = [&](const char* key, float& out) {
                const size_t n = std::strlen(key);
                if (s.compare(0, n, key) == 0) out = std::stof(s.substr(n));
            };
            if (s.rfind("--", 0) != 0) { a.mode = s; continue; }
            val("--speed=", a.speed);   val("--accel=", a.accel);   val("--brake=", a.brake);   val("--brakeFriction=", a.brakeFriction);
            val("--rifle=", a.rifle);   val("--pistol=", a.pistol); val("--shotgun=", a.shotgun);
        }
        return a;
    }

    // ── 각본 이동 — 한 구간(직선)을 가속·등속·제동으로 간다 ──
    //   Lyra 이동 부품과 같은 가속·제동 값을 써야 「따라가는 쪽의 뒤처짐」이 각본 탓인지 방법 탓인지 갈린다(플랜 1단계).
    //   Lyra 의 실제 제동은 마찰(BrakingFriction 6)까지 더해져 이 값보다 세다 — 서버는 감속을 상수 하나로만 둔다.
    struct Leg { Vec2 from, to; float idleAfter; };

    struct Mover
    {
        std::vector<Leg> legs;
        size_t legIndex = 0;
        float  s = 0, v = 0;          // 구간 안 거리 · 속력
        float  idleLeft = 0;          // 구간 끝 멈춤 남은 시간
        bool   started = false, done = false;
        Vec2   pos;
        float  yawDeg = 0;
        MovePhase phase = MovePhase::Idle;

        void Step(float dt, const Args& a)
        {
            if (done || legs.empty()) { phase = MovePhase::Idle; return; }
            Leg& leg = legs[legIndex];
            const Vec2  d   = leg.to - leg.from;
            const float L   = Len(d);
            const Vec2  dir = d * (1.0f / L);
            yawDeg = std::atan2(dir.y, dir.x) * DEG;

            if (idleLeft > 0)
            {
                idleLeft -= dt;
                phase = MovePhase::Idle;
                if (idleLeft <= 0) { idleLeft = 0; s = 0; v = 0; if (++legIndex >= legs.size()) done = true; }
                return;
            }

            // 감속 = 제동 + 마찰 × 속도 → 멈출 거리 = v/b − (a/b²)·ln(1 + b·v/a)  (a = 제동, b = 마찰. b = 0 이면 v²/2a)
            const float remain = L - s;
            const float b = a.brakeFriction;
            const float stopDist = (b > 0.f) ? v / b - (a.brake / (b * b)) * std::log(1.f + b * v / a.brake) : v * v / (2.f * a.brake);
            if (remain <= stopDist) { v = std::max(v - (a.brake + b * v) * dt, 0.f); phase = MovePhase::Brake; }
            else if (v < a.speed)                  { v = std::min(v + a.accel * dt, a.speed); phase = MovePhase::Accel; }
            else                                   { phase = MovePhase::Cruise; }

            s += v * dt;
            // 이산 적분이라 제동 끝에 몇 cm 모자라거나 넘칠 수 있다 — 속력이 0 이 되거나 끝을 지나면 끝점에 맞춘다
            if (s >= L || v <= 0.f)
            {
                s = L; v = 0;
                idleLeft = leg.idleAfter;
                phase = MovePhase::Idle;
                if (idleLeft <= 0) { s = 0; if (++legIndex >= legs.size()) done = true; }
            }
            pos = leg.from + dir * s;
        }

        // 입력 가속 — 가속·등속이면 진행 방향으로 「키를 누름」, 제동·멈춤이면 뗌
        Vec2 InputAccel(const Args& a) const
        {
            if (done || legs.empty() || (phase != MovePhase::Accel && phase != MovePhase::Cruise)) return {};
            const Leg& leg = legs[legIndex];
            const Vec2 d = leg.to - leg.from;
            return d * (a.accel / Len(d));
        }

        Vec2 Velocity() const
        {
            if (done || legs.empty() || v <= 0) return {};
            const Leg& leg = legs[legIndex];
            const Vec2 d = leg.to - leg.from;
            return d * (v / Len(d));
        }
    };
}

class CPilotServer
{
public:
    CPilotServer(const Args& a) : _args(a), _net(PORT, 4, ServerMode::GameServer, _monitor) {}

    bool Start() { return _net.Start(); }
    void Stop()  { _net.ShutdownServer(); }

    // 반환 false = 끝(Lyra 가 나갔고 각본도 끝났다)
    bool Tick(double nowSec)
    {
        std::queue<NetworkEvent> events;
        _net.SwapOutNetworkEvents(events);
        while (!events.empty())
        {
            NetworkEvent& e = events.front();
            switch (e.type)
            {
            case NetworkEvent::Type::CONNECTED:
                std::printf("[접속] session=%lld\n", (long long)e.sessionId);
                _session = e.sessionId;
                break;
            case NetworkEvent::Type::DISCONNECTED:
                std::printf("[끊김] session=%lld\n", (long long)e.sessionId);
                if (e.sessionId == _session) { _session = -1; _quit = true; }
                break;
            case NetworkEvent::Type::RECEIVED:
                OnPacket(e.sessionId, e.pMsg->GetReadBufferPtr(), e.pMsg->GetDataSize(), nowSec);
                e.pMsg->SubRef();
                break;
            }
            events.pop();
        }

        if (_helloAt >= 0) RunScript(nowSec);
        _net.FlushPendingSends();
        return !_quit;
    }

private:
    template <class T> void Send(T& pkt, Msg type, int size = sizeof(T))
    {
        pkt.h.size = static_cast<uint16_t>(size);
        pkt.h.type = static_cast<uint16_t>(type);
        if (_session >= 0) _net.RequestSendRaw(_session, reinterpret_cast<const char*>(&pkt), size);
    }

    void OnPacket(int64_t, const char* p, int size, double nowSec)
    {
        if (size < (int)sizeof(Header)) return;
        Header h; std::memcpy(&h, p, sizeof(h));
        switch (static_cast<Msg>(h.type))
        {
        case Msg::C2S_HELLO:
        {
            Hello m; std::memcpy(&m, p, sizeof(m));
            std::printf("[HELLO] 내 캐릭터 (%.0f, %.0f, %.0f) yaw=%.1f\n", m.x, m.y, m.z, m.yaw);
            _helloAt = nowSec;
            _local = { m.x, m.y }; _localZ = m.z; _localYaw = m.yaw;
            BuildScript();
            break;
        }
        case Msg::C2S_HIT:
        {
            Hit m; std::memcpy(&m, p, sizeof(m));
            OnHit(m);
            break;
        }
        default:
            std::printf("[수신] 모르는 type=%u size=%d\n", h.type, size);
        }
    }

    // 원격 캐릭터 경로는 내 캐릭터 앞에 깐다 — 카메라에 담기고, 맵 모양을 몰라도 빈 바닥일 공산이 크다
    void BuildScript()
    {
        const float r = _localYaw / DEG;
        Vec2 fwd   = { std::cos(r), std::sin(r) };
        Vec2 right = { -fwd.y, fwd.x };
        _origin = _local + fwd * 700.f + right * -300.f;
        if (_args.mode == "move")
        {
            // 이동 경로는 L_ShooterGym 의 고정 구역 — 내 캐릭터 자리 기준으로 깔았더니 출발점마다 장애물에 걸렸다(10-25 두 번째 실행, 오차 405cm 에서 멈춤).
            //   이 사각형(x -20~380, y -1370~-770)은 첫 실행에서 막힘 없이 돈 구역이고, 내 캐릭터 출발점 둘(x -720·-160, y -1070) 모두에서 정면(yaw 0)에 보인다
            _origin = { -20.f, -1370.f };
            fwd = { 1.f, 0.f };
            right = { 0.f, 1.f };
        }

        SpawnRemote sp{};
        sp.id = REMOTE_ID; sp.x = _origin.x; sp.y = _origin.y; sp.z = _localZ; sp.yaw = _localYaw + 180.f;
        Send(sp, Msg::S2C_SPAWN_REMOTE);
        std::printf("[각본] 모드=%s 원점 (%.0f, %.0f)\n", _args.mode.c_str(), _origin.x, _origin.y);

        if (_args.mode == "move")
        {
            const Vec2 A = _origin, B = A + right * 600.f, C = B + fwd * 400.f, D = A + fwd * 400.f;
            _mover.legs = { {A, B, 1.5f}, {B, C, 1.5f}, {C, D, 1.5f}, {D, A, 2.0f} };
            _mover.pos = A;
        }
        else if (_args.mode == "weapon")
        {
            WeaponTable t{};
            const WeaponEntry e[] = { {(uint8_t)Weapon::Rifle, _args.rifle}, {(uint8_t)Weapon::Pistol, _args.pistol}, {(uint8_t)Weapon::Shotgun, _args.shotgun} };
            t.count = 3;
            std::memcpy(t.entries, e, sizeof(e));
            Send(t, Msg::S2C_WEAPON_TABLE, (int)(sizeof(Header) + 1 + sizeof(WeaponEntry) * t.count));
            std::printf("[무기 표] 소총 %.0f · 권총 %.0f · 산탄총 %.0f\n", _args.rifle, _args.pistol, _args.shotgun);
        }
        _hp[Pilot::LOCAL_PLAYER_ID] = 100.f;
        _hp[REMOTE_ID] = 100.f;
    }

    void RunScript(double nowSec)
    {
        const double t = nowSec - _helloAt;
        if (_ended) return;

        if (_args.mode == "move")
        {
            if (t < 3.0) return;   // 원격 캐릭터 초기화 여유
            _mover.Step((float)TICK_SEC, _args);
            const Vec2 vel = _mover.Velocity();
            RemoteMove m{};
            m.id = REMOTE_ID; m.tick = ++_tick;
            m.x = _mover.pos.x; m.y = _mover.pos.y; m.z = _localZ;
            m.yaw = _mover.yawDeg; m.vx = vel.x; m.vy = vel.y;
            m.phase = (uint8_t)_mover.phase;
            const Vec2 acc = _mover.InputAccel(_args);
            m.ax = acc.x; m.ay = acc.y;
            Send(m, Msg::S2C_REMOTE_MOVE);
            if (_mover.done) End(t);
        }
        else if (_args.mode == "damage")
        {
            // 8초부터 2초마다 — 원격 30×4(→사망), 내 캐릭터 30×3(100·70·40·10)
            struct Step { double at; int32_t target; float amount; };
            static const Step steps[] = {
                { 8.0, REMOTE_ID, 30.f }, { 9.0, Pilot::LOCAL_PLAYER_ID, 30.f },
                {10.0, REMOTE_ID, 30.f }, {11.0, Pilot::LOCAL_PLAYER_ID, 30.f },
                {12.0, REMOTE_ID, 30.f }, {13.0, Pilot::LOCAL_PLAYER_ID, 30.f },
                {14.0, REMOTE_ID, 30.f },
            };
            while (_stepIndex < std::size(steps) && t >= steps[_stepIndex].at)
            {
                ApplyDamage(steps[_stepIndex].target, steps[_stepIndex].amount, "각본");
                ++_stepIndex;
            }
            if (t >= 18.0) End(t);
        }
        else   // weapon · idle — 맞힘 보고만 받는다
        {
            if (t >= 30.0) End(t);
        }
    }

    void OnHit(const Hit& m)
    {
        float dmg = 0;
        switch ((Weapon)m.weapon)
        {
        case Weapon::Rifle:   dmg = _args.rifle;   break;
        case Weapon::Pistol:  dmg = _args.pistol;  break;
        case Weapon::Shotgun: dmg = _args.shotgun; break;
        default: break;
        }
        std::printf("[맞힘] seq=%u 대상=%d 무기=%u → 표 피해 %.1f\n", m.seq, m.targetId, m.weapon, dmg);
        if (_args.mode == "weapon" && dmg > 0) ApplyDamage(m.targetId, dmg, "맞힘");
    }

    void ApplyDamage(int32_t target, float amount, const char* why)
    {
        float& hp = _hp[target];
        if (hp <= 0) return;   // 이미 죽음 — 서버가 정본이라 더 깎지 않는다
        hp = std::max(hp - amount, 0.f);
        Damage d{};
        d.targetId = target; d.amount = amount; d.hpAfter = hp; d.seq = ++_damageSeq;
        Send(d, Msg::S2C_DAMAGE);
        std::printf("[피해] seq=%u 대상=%d %.1f → 서버 체력 %.1f (%s)\n", d.seq, target, amount, hp, why);
    }

    void End(double t)
    {
        ScriptEnd e{};
        Send(e, Msg::S2C_SCRIPT_END);
        _ended = true;
        std::printf("[각본 끝] t=%.2fs 보낸 이동 %u · 피해 %u\n", t, _tick, _damageSeq);
    }

    Args            _args;
    CMonitorManager _monitor;
    NetIoModel      _net;
    int64_t         _session = -1;
    bool            _quit = false, _ended = false;
    double          _helloAt = -1;
    Vec2            _local, _origin;
    float           _localZ = 0, _localYaw = 0;
    Mover           _mover;
    uint32_t        _tick = 0, _damageSeq = 0;
    size_t          _stepIndex = 0;
    std::map<int32_t, float> _hp;
};

int main(int argc, char** argv)
{
    shared::LoggerGuard loggerGuard;
    const Args args = ParseArgs(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // 러너가 파일로 받는다 — 줄마다 바로 보이게

    CPilotServer server(args);
    if (!server.Start()) { std::printf("[오류] 서버 시작 실패 (포트 %d)\n", PORT); return 1; }
    std::printf("[시작] 포트 %d 모드=%s 속도 %.0f 가속 %.0f 제동 %.0f\n", PORT, args.mode.c_str(), args.speed, args.accel, args.brake);

    const auto t0 = Clock::now();
    auto next = t0;
    for (;;)
    {
        const double now = std::chrono::duration<double>(Clock::now() - t0).count();
        if (!server.Tick(now) || g_shutdownRequested) break;
        if (now > 600.0) { std::printf("[시간 초과] 10분\n"); break; }   // 붙는 쪽이 안 오면 혼자 끝낸다
        next += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(next);
    }
    server.Stop();
    std::printf("[종료]\n");
    return 0;
}
