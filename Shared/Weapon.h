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
		const char* name;				// 클립 파일의 묶음 이름과 같다 — 서버 · 화면 · 봇이 이 이름으로 무기별 자세 묶음을 고른다
		float baseDamage;
		float weakSpotMultiplier;
		float fireDelay;				// GA_Weapon_Fire 의 FireDelayTimeSecs
		float autoRate;					// 발사 몽타주 재생 속도 — 서버 애니 상태가 쏘기 몽타주를 이 속도로 튼다(9단계 조각 4, 9-1 기록이 준 속도와 같음)
		float reloadRate;				// 재장전 몽타주 재생 속도 — 서버 애니 상태가 재장전 몽타주를 이 속도로 튼다. 9-1 기록 권총 1.5 · 소총 · 샷건 1.0(실측)
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
		float meleeAbilitySeconds;		// 근접 능력이 켜져 있는 시간 = 근접 몽타주 길이 ÷ 자산 배율 − 빼기 시간. GA_Melee 는 몽타주가 빠지기 시작하면 끝난다(E-3)
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
		// 배율 몫은 그대로 두고 열만 덮는다 — 구간 첫 줄부터 배율만 굴려 온 뒤 대조 시작 줄의 열로 맞출 때
		void SetHeatForCheck(float heat);

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

	// ─── 조준 카메라 섞임 ─────────────────────────────────────
	// 언리얼 ULyraCameraModeStack(LyraCameraMode.cpp:146-213 · :264-328 · :365-405 · :449-460)을 두 모드(기본 · 조준)로 옮겼다. 자료형(float)까지 그대로.
	// - 왜 서버에: 사격 퍼짐이 읽는 조준 비율(WeaponMotion::aimingAlpha)이 이 스택에서 나온다(LyraRangedWeaponInstance.cpp:196-203)
	// - 그 값은 스택 「맨 끝」 모드가 조준 카메라면 1, 아니면 0 이다(맨 끝은 늘 비중 1). 그래서 섞이는 중간값은 퍼짐에 안 쓰이고,
	//   켤 때는 조준 모드가 다 섞여 아래 모드가 빠지는 순간 1 로, 풀 때는 기본 모드가 다 섞이는 순간 0 으로 뛴다(0-3 실행 기록 — 켠 뒤 14프레임 · 푼 뒤 30프레임)
	// - 덜 섞인 채 풀면 기본 모드가 스택 안에서 다시 위로 올라오며 맨 끝이 조준 모드가 된다 → 그 순간 1 이 된다(PushCameraMode 의 맨 끝 비중 1)
	class CAimCamera
	{
	public:
		enum EMode : uint8_t { MODE_THIRD_PERSON, MODE_ADS, MODE_COUNT };

		void Reset();								// 새 몸 — 스택이 빈 채 기본 모드가 들어온다(섞임 없이 비중 1)
		void Tick(bool adsActive, float deltaTime);	// 한 프레임 = 원하는 모드 넣기(PushCameraMode) → 섞기(UpdateStack)
		float GetAimingAlpha() const;				// 사격이 읽는 값

		// 대조 도구용
		// 기록한 스택(모드 수 · 맨 앞이 조준인가 · 맨 앞 비중)에서 시작한다. 모드가 둘뿐이라 둘일 때 맨 끝은 맨 앞의 반대 · 비중 1
		void SetForCheck(int count, bool topAds, float topWeight);
		int GetCount() const { return _count; }
		bool IsTopAds() const { return _count > 0 && _stack[0] == MODE_ADS; }
		float GetTopWeight() const { return _count > 0 ? _modes[_stack[0]].weight : 1.f; }

	private:
		struct Mode
		{
			float blendTime = 0;
			float exponent = 0;
			float alpha = 0;
			float weight = 0;
		};
		void Push(EMode mode);
		void SetWeight(Mode& m, float weight) const;
		void UpdateBlending(Mode& m, float deltaTime) const;

		Mode _modes[MODE_COUNT];
		uint8_t _stack[MODE_COUNT] = {};	// 0 = 맨 앞(새로 들어온 쪽)
		int _count = 0;
		bool _init = false;
	};

	// ─── 조준 능력 ─────────────────────────────────────────
	// GA_ADS 켜짐 한 사람치 — 서버 ApplyInput 과 내 예측(CSelfPrediction)이 같은 것을 쓴다(C-4, D61).
	// - 입력 차례: Apply(그 입력의 BUTTON_ADS) → 걸음(AdsWalkSpeed) → 무기 틱(퍼짐은 앞 입력의 카메라 값) → 카메라 Tick(active) → 애니 · STATE_ADS 도 active
	//   Lyra 는 키를 그 프레임 입력 처리에서 능력에 넘기고, 걸음 · 카메라 · 애니가 같은 프레임에 바뀐 능력을 본다(원본 기준 차례).
	//   기록 도구들은 프레임 끝에 눌러 한 프레임 늦게 보였다(C-1 · C-2 · C-3 실측의 「한 줄 앞」이 그것, D61)
	// - 새 몸의 첫 입력은 능력이 아직 안 붙어 누름이 버려진다(C-2 실측 — 누른 채 다시 태어나면 첫 프레임 600, 다음부터 300)
	struct AdsAbility
	{
		bool held = false;		// 마지막 입력의 버튼 — 입력이 끊긴 틱에 서버가 채우는 빈 입력도 누른 채로 둔다
		bool active = false;	// 이번 입력에 켜져 있나
		bool granted = false;	// 새 몸에 능력이 붙었나 — 첫 입력을 받은 뒤 참

		void Reset() { *this = AdsAbility(); }	// 새 몸 · 죽음(Lyra 죽음 능력이 다른 능력을 끈다)
		void Apply(bool buttonHeld)
		{
			held = buttonHeld;
			active = buttonHeld && granted;
			granted = true;
		}
	};

	constexpr float kAdsWalkSpeedMultiplier = 0.5f;	// GA_ADS ADSMultiplier(기본 객체) — 켤 때 MaxWalkSpeed 에 곱하고 풀면 되돌린다(C-2 실측 600 → 300)
	inline float AdsWalkSpeed(float maxWalkSpeed, bool adsActive) { return adsActive ? maxWalkSpeed * kAdsWalkSpeedMultiplier : maxWalkSpeed; }

	// ─── 발사 순환 ─────────────────────────────────────────

	struct WeaponInput
	{
		bool fireHeld = false;
		bool reloadPressed = false;
		bool meleeActive = false;	// 근접 능력이 켜져 있다 — 사격 · 재장전 입력 · 자동 재장전이 막힌다(TagRelationships_ShooterHero 의 근접 막기, E-3)
		bool meleeStarted = false;	// 이 입력에 근접 능력이 켜졌다 — 도는 재장전을 끊는다(같은 표의 근접 끊기)
	};

	struct WeaponEvents
	{
		bool fired = false;			// 이번 틱에 한 번 발사(탄 소비) — 한 발에 탄 수만큼 광선을 쏜다
		bool reloaded = false;		// 이번 틱에 탄이 들어옴
		bool reloadStarted = false;
		bool reloadCancelledByMelee = false;	// 이번 입력에 켜진 근접이 도는 재장전을 끊었다 — 탄이 안 들어간다
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
	// 근접(E-3)은 부르는 쪽이 이 순환 앞에서 켠다(CMeleeAbility) — 켜진 동안 발사 · 재장전 입력 · 자동 재장전이 막히고, 켜지는 입력에 도는 재장전이 끊긴다.
	//   발사 능력은 근접을 막지도 끊지도 않는다(관계표에 없음). 같은 입력에 둘 다 누르면 근접이 먼저라 발사가 막힌다 — Lyra 의 같은 프레임 차례는 확인 못 했다(제 판단)
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

	// ─── 근접 ──────────────────────────────────────────────
	// 값 = GA_Melee · GE_Damage_Melee 기본 객체(content-add-progress.md 0-3 표 · GA_Melee_cdo.t3d), 판정 차례 = GA_Melee 그래프, 확인 = Lyra 실행 기록(E-1 e1g1 · e1g3)

	inline constexpr double kMeleeDistance = 350.0;		// 쓸기 길이 — 시작 = hand_r 뼈, 끝 = 시작 + 몸 앞 × 이 값
	inline constexpr double kMeleeRadius = 96.0;		// 쓸기 구 반지름. 에셋은 반지름 96 · 반높이 48 캡슐인데 반높이가 반지름보다 작아 축이 0 = 구다(e1g1 원통 거리 37개가 구 해석값과 0.002cm 안)
	inline constexpr float kMeleeDamage = 100.f;		// 기본 피해. 거리 · 재질 보정 없음(0-3 실행 기록 — 앞 150~520 아홉 번 모두 100)
	inline constexpr double kMeleePushStrength = 2200.0;	// GA_Melee Strength — 맞히면 공격자를 몸 앞으로 이 값 × (맞은 거리 ÷ kMeleeDistance) 초속으로 민다(컨텐츠 E-2-1)
	inline constexpr float kMeleePushDuration = 0.05f;		// GA_Melee Duration — 60fps 에서 3프레임. 그 뒤는 마지막 밀기 속도에서 보통 제동(e1g3 300 → 105.7cm · 400 → 269.0cm)

	// 근접이 보는 몸 하나. 캡슐(LyraPawnCapsule)과 메시 몸체(LyraPawnMesh)가 둘 다 대상 종류 Pawn 이라 둘 다 쓸기에 걸린다(e1g1 — 정면 상대는 메시가 먼저)
	struct MeleeTarget
	{
		BodyPlacement body;					// 캡슐 중심 · 요 · 자세(자세가 비면 서 있는 고정 포즈 몸체)
		double capsuleRadius = 35.0;
		double capsuleHalfHeight = 90.0;		// 앉으면 65
		bool sameTeam = false;
	};

	enum class EMeleeHit : uint8_t
	{
		None,		// 아무 몸에도 안 닿음
		SameTeam,	// 가장 먼저 닿은 몸이 같은 팀 — 피해 · 밀기 없음. 그 뒤의 다른 팀은 안 본다(GA_Melee 는 단일 쓸기)
		Blocked,	// 손 → 닿은 점 선이 무기 채널 맵에 막힘
		Hit,
	};

	struct MeleeResult
	{
		EMeleeHit kind = EMeleeHit::None;
		uint32_t targetIndex = 0;	// targets 번호(None 이 아닐 때)
		double distance = 0;		// 쓸기 시작 → 처음 닿은 때 구 중심(언리얼 FHitResult::Distance). 처음부터 겹치면 0. 밀기 세기가 이 값에 비례한다
		Vec3 location;				// 처음 닿은 때 구 중심
		Vec3 impactPoint;			// 닿은 점 — 벽 광선의 끝
		bool capsule = false;		// 캡슐에 닿음. 거짓이면 메시 몸체(shape 번호)
		uint32_t shape = 0;
	};

	// GA_Melee 차례 그대로 — 구를 hand → hand + forward × kMeleeDistance 로 쓸어 가장 먼저 닿은 몸 하나 → 같은 팀이면 SameTeam →
	// hand → 닿은 점 선이 무기 채널 맵에 막히면 Blocked → 아니면 Hit.
	// hand = 앞 프레임 자세의 hand_r 뼈(능력은 그 프레임 자세 평가 전에 켜진다, e1g1). forward = 몸 앞 단위 벡터(요만). targets 에서 공격자 자신은 부르는 쪽이 뺀다.
	// - 같은 때 닿은 것이 여럿이면(처음부터 겹침 등) targets 앞의 몸, 한 몸 안에서는 메시 몸체를 캡슐보다 먼저 친다. 엔진이 고르는 차례는 확인 못 했다(e1g1 앞 150 은 메시)
	// - 벽 광선을 다른 몸이 막는 경우는 안 본다. 그 선은 쓸기 부피 안이라, 선에 걸리는 몸은 쓸기에 먼저(또는 같은 때) 닿는다
	void ResolveMelee(const CMapCollision& map, const MeleeTarget* targets, size_t targetCount, const Vec3& hand, const Vec3& forward, MeleeResult& out);

	// GA_Melee 밀기 힘 — ApplyRootMotionConstantForce(방향 = 몸 앞, 세기 = Strength × NormalizeToRange(맞은 거리, 0, Distance)).
	// 맞은 거리는 FHitResult::Distance 라 float, 세기는 블루프린트 double 곱 뒤 능력 작업 인자 float 로 줄어든다 — 그 차례대로.
	// 부르는 쪽 = 맞음(Hit)일 때만(같은 팀 · 빗나감은 밀기 없음, 0-3 gym2). 넣기 = CMoveSim::ApplyRootForce(이동 상태, 이 값, kMeleePushDuration)
	Vec3 MeleePushForce(const Vec3& forward, double hitDistance);

	// 근접 손(hand_r)을 세계에 놓는다 — 몸 기준 손(요 0, CAnimPoseEvaluator::HandRightFromLocal)을 요로 돌려 캡슐 중심에 더한다(TraceBody 의 몸 → 세계와 같은 돌림).
	// 서버(CRoom::MeleeHandOf)와 봇 화면 판정이 같이 쓴다
	Vec3 MeleeHandWorld(const Vec3& location, float yawDegrees, const double localHand[3]);
	// 손을 구할 자세가 없을 때(애니 클립 없음 · 부활 첫 입력)의 몸 기준 손 — 권총 기본 자세 · 선 메시(MeleeCheck HAND 줄, 클립 묶음으로 계산한 값). 앉았으면 z 에 메시 올림을 더한다
	inline constexpr double kMeleeRestHandLocal[3] = { 37.792, 21.166, 52.242 };
	// 앉은 기본 자세 손 — 권총 앉기 묶음 비중 1 · 앉음 · 메시 올림 0(MeleeCheck HAND crouch=1 줄). 선 손에 올림만 더하면 앉은 자세 손과 68.6cm 어긋났다(17-b2 q0)
	inline constexpr double kMeleeRestHandCrouchLocal[3] = { 41.229, 16.321, -14.304 };
	// 자세가 없을 때의 몸 기준 손 — 앉았으면 앉은 기본 손에 메시 올림을 더한다. MeleeCheck 가 두 상수를 클립 계산값과 견준다(REST 줄)
	inline void MeleeRestHandLocal(bool crouched, double crouchMeshLiftZ, double out[3])
	{
		const double* rest = crouched ? kMeleeRestHandCrouchLocal : kMeleeRestHandLocal;
		out[0] = rest[0];
		out[1] = rest[1];
		out[2] = rest[2] + (crouched ? crouchMeshLiftZ : 0.0);
	}

	// 근접 능력이 켜져 있는 입력 수 = 켜진 시간 ÷ 틱을 올림. 몽타주가 빠지기 시작하는 프레임 안(입력 처리 뒤)에서 끝나 그다음 입력에서 다시 켜진다 —
	// 연타 기록(e1g3 spam_*)의 다시 켜지는 간격 권총 34 · 소총 35 · 샷건 46 프레임과 같다(실측)
	uint32_t MeleeAbilityInputs(const WeaponDef& def, float tickSeconds);

	// 근접 능력(GA_Melee)이 켜져 있나 — 틱이 아니라 입력 개수로 센다(발사 간격 · 이동 막음과 같은 까닭: 클라가 같은 입력을 돌려 보면 같은 결과).
	// 켜진 동안 누름은 버린다(bRetriggerInstancedAbility = False). 서버 · 내 화면 예측 · 대조 도구가 같이 쓴다.
	// 차례 = 입력마다 NextInput → (누름이 있으면) TryActivate → 무기 순환(WeaponInput 의 근접 두 칸)
	class CMeleeAbility
	{
	public:
		void NextInput() { if (_inputsLeft > 0) --_inputsLeft; }
		// 꺼져 있으면 켠다. 켰으면 true. 켜는 입력도 켜진 입력 수에 든다
		bool TryActivate(const WeaponDef& def, float tickSeconds)
		{
			if (_inputsLeft > 0) return false;
			_inputsLeft = MeleeAbilityInputs(def, tickSeconds);
			return true;
		}
		bool IsActive() const { return _inputsLeft > 0; }
		// 다음 입력에서 누르면 켜지나 — 부르는 쪽이 켜질 입력에만 화면 근접 판정을 하려고 입력을 넣기 전에 본다
		bool WillActivateNextInput() const { return _inputsLeft <= 1; }
		uint32_t GetInputsLeft() const { return _inputsLeft; }
		void Reset() { _inputsLeft = 0; }

	private:
		uint32_t _inputsLeft = 0;	// 이 입력을 포함해 켜져 있을 입력 수. 0 = 꺼짐
	};
}
