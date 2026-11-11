#pragma once

#include "MapCollision.h"

#include <cstddef>
#include <cstdint>

// 무기: 수치 표, 피해 계산, 퍼짐, 발사 순환(탄창·재장전), 몸 판정, 한 발의 결과.
// - 왜: 사격 판정은 서버 일이다. 정답지는 Lyra 실행값이고 Tools/WeaponCheck 가 기록과 대조한다.
// - 언리얼 안에서도 컴파일돼야 하므로 예외·RTTI 를 쓰지 않는다. 네트워크·엔진을 모른다.
namespace mo
{
	enum class EWeapon : uint8_t { Pistol, Rifle, Shotgun, Count };

	enum class ECurveInterp : uint8_t { Linear, Constant };

	struct CurveKey
	{
		float time;
		float value;
		ECurveInterp interp;	// 이 키에서 다음 키까지
	};

	// 언리얼 FRichCurve 중 Lyra 무기가 쓰는 것만 — 범위 밖은 끝 값 그대로, 키는 선형 또는 상수
	struct Curve
	{
		const CurveKey* keys = nullptr;
		uint32_t count = 0;

		float Eval(float time) const;
		float MinTime() const;
		float MaxTime() const;
		float MinValue() const;
		float MaxValue() const;
	};

	struct WeaponDef
	{
		const char* name;
		float baseDamage;
		float weakSpotMultiplier;
		float fireDelay;				// GA_Weapon_Fire 의 FireDelayTimeSecs
		float autoRate;					// 발사 몽타주 재생 속도 — 판정엔 안 쓰이고 대조 표에만 있다
		int32_t magazineSize;
		int32_t startSpareAmmo;
		int32_t bulletsPerCartridge;
		float maxDamageRange;
		float spreadExponent;
		float sweepRadius;
		bool firstShotAccuracy;
		float aimingMultiplier;
		float standingStillMultiplier;
		float standingStillRate;
		float standingStillSpeedThreshold;
		float standingStillSpeedRange;
		float crouchingMultiplier;
		float crouchingRate;
		float jumpFallMultiplier;
		float jumpFallRate;
		float recoveryCooldownDelay;	// 표에만 있다 — Lyra 에서 효과가 없다(CWeaponSpread::Tick 주석)
		float fireMontageSeconds;		// 발사 몽타주 길이 ÷ 재생 속도 — FireDelay 보다 먼저 끝나면 이쪽이 연사 간격을 정한다(샷건)
		float reloadSeconds;			// 재장전 시작부터 탄이 들어오기까지 — 몽타주 안 AN_Reload 위치 ÷ 재생 속도
		float autoReloadPollSeconds;	// GA_Weapon_AutoReload 의 PollInterval
		float autoReloadIdleSeconds;	// GA_Weapon_AutoReload 의 TimeSinceActivityToReload
		Curve heatToSpread;
		Curve heatToHeatPerShot;
		Curve heatToCooldownPerSecond;
		Curve damageFalloff;
	};

	const WeaponDef& GetWeaponDef(EWeapon weapon);

	// 부위·거리·팀까지 포함한 한 발 피해. 언리얼 LyraDamageExecution.cpp 의 공식과 같다(float)
	float ComputeDamage(const WeaponDef& def, double distance, bool weakSpot, bool sameTeam);

	// ─── 퍼짐 ─────────────────────────────────────────────

	struct WeaponMotion
	{
		float speed = 0;			// 수평·수직 합친 속도(cm/s) — 언리얼 Pawn->GetVelocity().Size()
		bool falling = false;
		bool crouching = false;
		float aimingAlpha = 0;		// 조준 카메라 섞임 정도(0~1). 첫 판에는 조준이 없어 0
	};

	// 언리얼 ULyraRangedWeaponInstance 의 퍼짐 계산(자료형까지 그대로)
	class CWeaponSpread
	{
	public:
		void OnEquipped(const WeaponDef& def);
		void Tick(float deltaTime, const WeaponMotion& motion);
		void AddSpread();

		float GetHeat() const { return _heat; }
		float GetSpreadAngle() const { return _spreadAngle; }							// 도. 전체 각
		float GetCalculatedMultiplier() const { return _firstShot ? 0.f : _multiplier; }
		bool HasFirstShotAccuracy() const { return _firstShot; }

		// 대조 도구용 — 기록 도중부터 시작할 때
		void SetForCheck(const WeaponDef& def, float heat, float standingStillMultiplier);

	private:
		float ClampHeat(float heat) const;

		const WeaponDef* _def = nullptr;
		float _heat = 0;
		float _spreadAngle = 0;
		float _multiplier = 1;
		float _standing = 1;
		float _crouching = 1;
		float _jumpFall = 1;
		bool _firstShot = false;
	};

	// ─── 발사 순환 ─────────────────────────────────────────

	struct WeaponInput
	{
		bool fireHeld = false;
		bool reloadPressed = false;
	};

	struct WeaponEvents
	{
		bool fired = false;			// 이번 틱에 한 번 발사(탄 소비) — 한 발에 탄 수만큼 광선을 쏜다
		bool reloaded = false;		// 이번 틱에 탄이 들어옴
		bool reloadStarted = false;
		float spreadAngle = 0;			// fired 일 때 — 이번 발의 퍼짐. 이번 발이 더하는 열은 들어가지 않는다(Lyra 는 광선을 쏜 뒤 열을 더한다)
		float spreadMultiplier = 1;
	};

	// 프레임 안 순서: 발사 입력 → 재장전 입력 → 퍼짐 계산 → 타이머 → 자동 재장전 검사.
	// 이 순서 자체가 규칙이다(전부 Lyra 기록 weaponrec_*.csv 의 ABILITY·FRAME 줄에서 확인):
	// - 발사가 재장전 입력보다 먼저 → 같은 틱에 둘 다 누르면 발사가 켜지고 재장전은 막힌다
	// - 발사가 시작되면 진행 중인 재장전은 취소된다 — 빈 탄창에서 시작한 재장전은 탄이 없어 발사가 안 되므로 취소되지 않는다.
	//   Lyra 의 Ability.Weapon.NoFiring 은 그 구간과 겹쳐 결과를 바꾸지 않으므로 따로 들지 않는다(빼도 기록과 같음을 확인)
	// - 수동 재장전은 시작한 틱부터 시간이 줄고, 자동 재장전은 타이머 뒤에 시작해 다음 틱부터 준다(Lyra 에서 한 프레임 차이)
	// 시간은 초로 들고 간다 — Lyra 는 프레임에 묶이므로 고정 틱에서 같은 간격이 나온다
	class CWeaponCycle;

	// 무기를 바꾸면 Equip 이 발사 타이머를 지우므로, 번갈아 바꾸면 연사 제한을 건너뛸 수 있다 — 무기별로 다음에 쏠 수 있는 입력 번호를 따로 기억한다.
	// - 왜 입력 개수로 세나: 클라가 같은 입력을 돌려 보면 같은 결과가 나와야 한다(이동 막음과 같은 까닭). 평균 빠르기는 서버의 입력 한도가 틱에 묶는다
	// - 내림으로 잡아 무기를 안 바꾼 정직한 간격은 막지 않는다. 연사 간격은 FireDelay 와 몽타주 중 먼저 끝나는 쪽
	class CWeaponFireGate
	{
	public:
		void NextInput() { ++_inputIndex; }
		bool IsBlocked(EWeapon weapon, bool fireActive) const { return !fireActive && _inputIndex < _readyInput[(size_t)weapon]; }
		void OnFired(EWeapon weapon, const WeaponDef& def, float tickSeconds)
		{
			const float seconds = def.fireDelay < def.fireMontageSeconds ? def.fireDelay : def.fireMontageSeconds;
			_readyInput[(size_t)weapon] = _inputIndex + (uint32_t)(seconds / tickSeconds + 1e-3f);
		}
		uint32_t GetInputIndex() const { return _inputIndex; }

	private:
		uint32_t _inputIndex = 0;
		uint32_t _readyInput[(size_t)EWeapon::Count] = {};
	};

	class CWeaponCycle
	{
	public:
		void Init(EWeapon weapon);
		void Init(const WeaponDef& def);	// 대조 도구가 값을 일부러 틀릴 때
		void Equip();
		WeaponEvents Tick(float deltaTime, const WeaponInput& input, const WeaponMotion& motion);

		int32_t GetMagazine() const { return _magazine; }
		int32_t GetSpare() const { return _spare; }
		bool IsReloading() const { return _reloadLeft > 0; }
		bool IsFireActive() const { return _fireActive; }
		const WeaponDef& GetDef() const { return *_def; }
		const CWeaponSpread& GetSpread() const { return _spread; }

		// 대조 도구용
		void SetAmmoForCheck(int32_t magazine, int32_t spare) { _magazine = magazine; _spare = spare; }
		CWeaponSpread& SpreadForCheck() { return _spread; }
		// 다음 틱부터 재장전 없이 쉬게 한다(구간 시작에 맞춘다)
		void ResetCycleForCheck() { _fireLeft = 0; _fireMontageLeft = 0; _fireActive = false; _reloadLeft = 0; _idleSeconds = 0; _pollLeft = _def->autoReloadPollSeconds; }
		// 자동 재장전 검사 주기는 장착 시각이 기준이다 — 기록 중간부터 이어 갈 때 맞춘다
		void SetAutoReloadPhaseForCheck(float pollLeft, float idleSeconds) { _pollLeft = pollLeft; _idleSeconds = idleSeconds; }

	private:
		void StartReload();

		const WeaponDef* _def = nullptr;
		CWeaponSpread _spread;
		int32_t _magazine = 0;
		int32_t _spare = 0;
		float _fireLeft = 0;		// 발사 능력이 끝나기까지 — FireDelay
		float _fireMontageLeft = 0;	// 발사 능력이 끝나기까지 — 몽타주. 둘 중 먼저 끝나는 쪽
		bool _fireActive = false;
		float _reloadLeft = 0;		// 탄이 들어오기까지
		float _idleSeconds = 0;		// 마지막 활동 후 시간 — 자동 재장전 조건
		float _pollLeft = 0;		// 자동 재장전 다음 검사까지
	};

	// ─── 난수와 퍼짐 방향 ──────────────────────────────────

	class CSpreadRng
	{
	public:
		explicit CSpreadRng(uint32_t seed = 1) : _state(seed ? seed : 1) {}
		float NextFloat();		// [0, 1)
	private:
		uint32_t _state;
	};

	// 언리얼 VRandConeNormalDistribution — 조준 방향(단위 벡터)에서 원뿔 안의 방향 하나.
	// 난수 값만 다르고(엔진 FRand 는 재현 불가) 분포와 회전 순서는 같다
	Vec3 SpreadDirection(const Vec3& aimDir, float spreadAngleDegrees, float multiplier, float exponent, CSpreadRng& rng);

	// 탄 방향의 난수 시작값 — 서버와 화면이 입력 번호와 탄 번호로 각자 같은 값을 얻는다(D15). 방향을 주고받지 않는다
	uint32_t SpreadSeed(uint32_t inputSeq, uint32_t bulletIndex);

	// 시선(요 · 피치, 도) → 단위 방향. 서버가 사격에 쓰는 식 그대로
	Vec3 AimDirection(float yawDegrees, float pitchDegrees);

	// ─── 몸 판정 ───────────────────────────────────────────

	enum class EBodyShape : uint8_t { Sphere, Sphyl, Box };

	// 서 있는 포즈의 몸체 하나. 좌표는 캡슐 중심 기준, 캐릭터 요 0 기준
	struct BodyShape
	{
		const char* bone;
		EBodyShape kind;
		bool weakSpot;
		float center[3];
		float axisX[3];
		float axisY[3];
		float axisZ[3];
		float a;	// 구·캡슐 = 반지름. 상자 = X 전체 길이
		float b;	// 캡슐 = 축 길이(양 끝 반구 제외). 상자 = Y 전체 길이
		float c;	// 상자 = Z 전체 길이
	};

	const BodyShape* GetBodyShapes(size_t& outCount);

	struct BodyPose;
	struct ClipBody;

	struct BodyPlacement
	{
		Vec3 center;		// 캡슐 중심
		double yawDegrees = 0;
		uint32_t id = 0;	// 호출하는 쪽이 정한 번호
		// 애니 자세(D32). 비어 있으면 서 있는 자세(GetBodyShapes)로 판정한다. 가리키는 값은 부른 쪽이 들고 있는다
		const BodyPose* pose = nullptr;
		const ClipBody* shapes = nullptr;
		uint32_t shapeCount = 0;
		bool skip = false;	// 이 탄이 몸 근처에도 안 온다 — 자세를 구하지 않고 건너뛴다
	};

	struct BodyHit
	{
		double time = 0;	// 0(시작)~1(끝)
		Vec3 point;			// 맞은 점
		uint32_t shape = 0;	// GetBodyShapes 번호. 자세를 줬으면 ClipBody 번호
		bool weakSpot = false;
	};

	// 고정 포즈 — 애니메이션이 없으므로 Lyra 화면과 다를 수 있다. 일치율은 Tools/WeaponCheck 가 잰다.
	// radius > 0 이면 구 스윕. 상자는 면을 radius 만큼 밀어 근사한다(모서리에서 조금 넓다)
	// body.pose 를 주면 고정 포즈 대신 그 자세로 판정한다
	bool TraceBody(const BodyPlacement& body, const Vec3& start, const Vec3& end, double radius, BodyHit& out);

	// ─── 한 발 ─────────────────────────────────────────────

	enum class EBulletHit : uint8_t { None, Map, Body };

	struct BulletResult
	{
		EBulletHit kind = EBulletHit::None;
		double distance = 0;		// 시작점 ↔ 맞은 점 — 피해 감쇠에 쓰는 값
		Vec3 point;
		uint32_t bodyIndex = 0;		// bodies 배열 번호
		uint32_t shape = 0;
		bool weakSpot = false;
		bool usedSweep = false;
	};

	// 언리얼 DoSingleBulletTrace 규칙:
	// 선을 먼저 쏴서 몸이 먼저면 그것, 아니면 구로 다시 쏴서 몸이 가장 먼저 닿으면 그것, 아니면 선 결과.
	// bodies 에서 쏜 사람 자신은 호출하는 쪽이 뺀다
	void ResolveBullet(const CMapCollision& map, const BodyPlacement* bodies, size_t bodyCount, const Vec3& start, const Vec3& end, double sweepRadius, BulletResult& out);
}
