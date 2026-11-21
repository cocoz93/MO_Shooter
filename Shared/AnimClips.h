#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Lyra 에서 뽑은 클립 자료를 읽어 서버 · 화면 · 봇이 같은 값으로 자세를 계산하게 한다.
// - 왜: 서버는 언리얼 없이 돌아야 한다. 클립은 Epic 샘플에서 나온 자료라 리포에 넣지 않고, 뽑는 도구(Tools/LyraPilot/make_clip_pack.py)와 이 읽는 코드만 둔다.
// - 언리얼 안에서도 컴파일돼야 하므로 예외·RTTI 를 쓰지 않는다. 불러온 뒤에는 읽기만 하므로 여러 스레드가 함께 읽어도 된다.
//
// 파일 형식(리틀 엔디언, 패딩 없음). make_clip_pack.py 와 짝이다. 고치면 kClipPackVersion 을 같이 올린다
//   'MOAC', u32 버전
//   u32 뼈 수, 뼈마다 { u8 이름 길이, 이름(ascii), i16 부모(남긴 뼈 기준, 뿌리 -1), 기준 자세 7f (px py pz qx qy qz qw) }
//   메시 자리 7f (px py pz qx qy qz qw) — 캐릭터 몸 기준에서 본 메시 뿌리
//   u32 부위 수, 부위마다 { u8 이름 길이, 뼈 이름, u8 모양(EBodyShape 값), u8 약점, 뼈 기준 자리 3f, 뼈 기준 회전 4f, 크기 a b c 3f }
//   u32 뼈별 비중 표 수(버전 2 — 골격의 블렌드 프로파일), 표마다 { u8 이름 길이, 이름, u8 방식(엔진 EBlendProfileMode 값), f[뼈 수] 비중(남긴 뼈 차례) }
//   u32 끼움 자리 수(버전 2 — 몽타주 줄이 들어가는 자리), 자리마다 { u8 이름 길이, 이름, u8 방식(ESlotMode), i16 뼈별 비중 표(-1 = 모든 뼈 1) }
//   u32 묶음 수, 묶음마다 {
//     u8 이름 길이, 이름, u32 표본 Hz, 6f 조준 각도(위 · 가운데 · 아래 · 느슨한 위 · 가운데 · 아래),
//     5f (groundFriction, brakingFriction, brakingFrictionFactor, brakingDecelerationWalking, gravityZ), u8 separateBraking,
//     u32 클립 수(앞 ACLIP_COUNT 개는 EAnimClip 차례 — 버전 3 부터 조준 클립 17개 · 버전 4 부터 앉기 클립 20개 포함, 그 뒤는 몽타주 · 덮기 자세 클립), 클립마다 {
//       u8 이름 길이, 이름, f 길이(초), u8 덧붙임 종류(0 = 일반), u8 반복, f rootSpeed, u32 표본 수,
//       u16 발 표시 수, 표시마다 { f 시각, u8 왼발 },
//       u8 곡선 유무, 있으면 { u32 hz, u32 값 수, f[값 수] },
//       키 = 표본 수 × 뼈 수 × 7f (px py pz qx qy qz qw) },
//     u32 몽타주 수(버전 2), 몽타주마다 {
//       u8 이름 길이, 이름, u8 종류(EMontageKind), u8 무리 이름 길이, 무리 이름, f 길이(초), f 재생 속도,
//       f 넣기 시간, u8 넣기 곡선(엔진 EAlphaBlendOption 값), f 빼기 시간, u8 빼기 곡선, f 빼기 시작(음수 = 끝에서 빼기 시간만큼 앞), u8 자동 빼기,
//       i16 빼기 비중 표(버전 6 — 뼈별 비중 표 번호, -1 = 없음),
//       u8 줄 수, 줄마다 { u8 끼움 자리, u16 클립, f 몽타주 시각, f 클립 시작, f 클립 끝, f 클립 재생 속도 },
//       u8 곡선 수, 곡선마다 { u8 이름 길이, 이름, i8 자리(-1 = 몽타주 시각 기준, 0 이상 = 그 줄 클립의 시각 기준), u32 hz, u32 값 수, f[값 수] } },
//     i16 쏜 뒤 윗몸 덮기 클립(그 클립 시각 0 의 자세를 쓴다. -1 = 없음), u8 그 끼움 자리 }
//   'MOAE'
// 크기 칸은 넣지 않는다. 일반 클립은 1, 덧붙임 클립은 0 으로 항상 같고, 다르면 도구가 파일을 만들지 않는다.
namespace mo
{
	inline constexpr uint32_t kClipPackVersion = 7;	// 7 = 몽타주 종류 12 → 13(수류탄 던짐, 컨텐츠 G-4). 칸 꼴은 그대로다. 6 은 거부한다(수류탄 몽타주가 없다).
														// 6 = 몽타주 종류 8 → 12(대시 앞 · 뒤 · 왼 · 오른, 컨텐츠 D-4) · 몽타주마다 빼기 비중 표 칸(대시 넷 = FastFeet). 5 는 거부한다(대시 몽타주가 없다).
														// 5 = 몽타주 종류 5 → 8(근접 · 맞은 쪽 반응 뒤 · 앞, 컨텐츠 E-2). 칸 꼴은 그대로다. 4 는 거부한다(근접 몽타주가 없다).
														// 4 = 조준 클립 뒤에 앉기 클립 20개(컨텐츠 D-3) — 몽타주 클립 번호가 20 씩 더 밀린다. 3 은 거부한다(앉기 클립이 없다).
														// 3 = 묶음 클립 30개 뒤에 조준 클립 17개(컨텐츠 C-3, D60) — 몽타주 클립 번호가 17 씩 밀린다. 2 는 거부한다(조준 클립이 없다).
														// 2 = 무기별 묶음 셋 + 뼈별 비중 표 · 끼움 자리 · 몽타주 표(9단계). 1 은 거부한다 — 소총까지 다시 만든다.
														// 2-a 에서 잠시 만든 「몽타주 칸이 빈」 판은 비중 표 칸이 없어 뒤 칸이 밀려 거부된다(BadSlot — 3-a 에서 그 파일로 확인)

	enum class EClipLoad
	{
		Ok,
		TooSmall,
		BadMagic,
		BadVersion,
		Truncated,
		BadBone,		// 뼈가 없거나 부모가 자기보다 뒤에 있다
		BadBody,		// 부위가 없거나 없는 뼈를 가리킨다
		BadBundle,		// 묶음이 없거나 표본 Hz 가 0 이다
		BadClip,		// 길이가 0 이하거나 표본 수가 길이 × Hz 와 맞지 않는다
		BadTail,		// 끝 표시가 다르거나 뒤에 남는 바이트가 있다
		BadSlot,		// 뼈별 비중 표 · 끼움 자리 표가 어긋난다(방식 · 표 번호가 범위 밖, 이름이 겹침)
		BadMontage,		// 몽타주 표가 어긋난다(종류가 범위 밖이거나 겹침, 줄이 없는 자리 · 클립을 가리킴, 클립 구간이 클립 밖, 덮기 자세가 없는 클립, 섞기 곡선이 kBlendOptionMax 밖)
	};

	// 끼움 자리가 아래 자세에 몽타주 자세를 넣는 방식
	enum class ESlotMode : uint8_t
	{
		Replace,		// 통째로 바꾼다(부활 FullBody)
		Additive,		// 덧붙인다(사격 FullBodyAdditivePreAim · 윗몸 덧붙임 UpperBodyAdditive · 맞은 쪽 반응 AdditiveHitReact)
		PerBone,		// 뼈별 비중으로 섞는다(윗몸 UpperBody)
		Count,
	};

	// 묶음의 몽타주 종류. 뽑는 쪽(MOBSimpleAnimInstance.cpp WEAPON_MONTAGES)과 같은 차례
	enum EMontageKind : uint8_t
	{
		MONTAGE_FIRE,
		MONTAGE_RELOAD,
		MONTAGE_EQUIP,		// 이 무기를 꺼낼 때
		MONTAGE_UNEQUIP,	// 앞 무기를 넣을 때(세 무기 같은 자산)
		MONTAGE_SPAWN,		// 부활(세 무기 같은 자산)
		MONTAGE_MELEE,		// 근접 공격(무기마다 자산 — GA_Melee 의 무기별 MeleeAttackMontage, e1g3 실측 권총 · 소총 · 샷건 셋)
		MONTAGE_HIT_BACK,	// 맞은 쪽 반응 AM_MM_HitReact_Back_Med_01(세 무기 같은 자산). 근접은 이것과 아래 것 중 무작위(D58)
		MONTAGE_HIT_FRONT,	// 맞은 쪽 반응 AM_MM_HitReact_Front_Med_02
		MONTAGE_DASH_FORWARD,	// 대시(컨텐츠 D-4) — GA_Hero_Dash 의 AM_MM_Dash_Forward · Backward · Left · Right, 자리 FullBody, 세 무기 같은 자산(D-0 dump_dash.txt, 원본 기준).
		MONTAGE_DASH_BACKWARD,	// 차례 = EDashDir(Shared/Dash.h) — 종류 = MONTAGE_DASH_FORWARD + 방향
		MONTAGE_DASH_LEFT,
		MONTAGE_DASH_RIGHT,
		MONTAGE_GRENADE,	// 수류탄 던짐(컨텐츠 G-4) — GA_Grenade 의 AM_MM_Rifle_GrenadeToss 0.8667초, 트랙 UpperBody · UpperBodyAdditive, 세 무기 같은 자산(G-0 dump_gren.txt, 원본 기준)
		MONTAGE_KIND_COUNT,
	};

	// 묶음 안의 클립 차례. 뽑는 쪽(MOBSimpleAnimInstance.cpp AppendClipDump)과 같다
	enum EAnimClip : uint32_t
	{
		ACLIP_IDLE,
		ACLIP_JOG_FWD,
		ACLIP_JOG_BWD,
		ACLIP_JOG_LEFT,
		ACLIP_JOG_RIGHT,
		ACLIP_AIM_UP,
		ACLIP_AIM_CENTER,
		ACLIP_AIM_DOWN,
		ACLIP_READY_UP,
		ACLIP_READY_CENTER,
		ACLIP_READY_DOWN,
		ACLIP_JUMP_START,
		ACLIP_JUMP_START_LOOP,
		ACLIP_JUMP_APEX,
		ACLIP_JUMP_FALL_LOOP,
		ACLIP_JUMP_LAND,
		ACLIP_TRANSITION_FIRST,								// 전환 12개 = 종류(시작 · 멈춤 · 방향 꺾기) × 4 + 방향(앞 · 뒤 · 왼 · 오른)
		ACLIP_RECOVERY = ACLIP_TRANSITION_FIRST + 12,		// 착지 충격(덧붙임)
		ACLIP_FIDGET,										// 대기 몸짓
		// 조준 묶음(버전 3, D60) — 위 서기 · 달리기 넷 · 전환 12 와 같은 차례의 조준 클립(Idle_ADS · Walk_*). 뽑는 쪽 = MOBSimpleAnimInstance.cpp WEAPON_ADS
		ACLIP_ADS_IDLE,
		ACLIP_ADS_WALK_FWD,
		ACLIP_ADS_WALK_BWD,
		ACLIP_ADS_WALK_LEFT,
		ACLIP_ADS_WALK_RIGHT,
		ACLIP_ADS_TRANSITION_FIRST,							// 조준 전환 12개 — 허리 전환과 같은 차례
		// 앉기 묶음(버전 4, D-3) — 서기 · 걷기 넷 · 전환 12 는 조준 묶음과 같은 차례(Crouch_Idle · Crouch_Walk_*). 그 뒤 서기 상태 안 앉기 · 일어서기 전환과
		// 앉아 쏜 뒤 덮기 자세(Aim_HipFirePose_Crouch). 뽑는 쪽 = MOBSimpleAnimInstance.cpp WEAPON_CROUCH
		ACLIP_CROUCH_IDLE = ACLIP_ADS_TRANSITION_FIRST + 12,
		ACLIP_CROUCH_WALK_FWD,
		ACLIP_CROUCH_WALK_BWD,
		ACLIP_CROUCH_WALK_LEFT,
		ACLIP_CROUCH_WALK_RIGHT,
		ACLIP_CROUCH_TRANSITION_FIRST,						// 앉은 전환 12개 — 허리 전환과 같은 차례
		ACLIP_CROUCH_ENTRY = ACLIP_CROUCH_TRANSITION_FIRST + 12,	// 서서 앉기(Crouch_Idle_Entry)
		ACLIP_CROUCH_EXIT,									// 앉았다 일어서기(Crouch_Idle_Exit)
		ACLIP_CROUCH_OVERRIDE,								// 앉아 쏜 뒤 윗몸 덮기 자세(시각 0 고정)
		ACLIP_COUNT,										// 67
	};
	static_assert(ACLIP_ADS_IDLE == 30 && ACLIP_CROUCH_IDLE == 47 && ACLIP_COUNT == 67, "뽑는 쪽(AppendClipDump)의 번호와 같아야 한다");

	// 쌓기 칸 번호(AnimLayer::clip) → 묶음 클립 번호. 0~16 = 점프 5 · 허리 전환 12(ACLIP_JUMP_START 부터 잇닿음), 17~28 = 조준 전환 12,
	// 29~42 = 앉은 전환 12 · 서기 안 앉기 · 일어서기 전환(ACLIP_CROUCH_TRANSITION_FIRST 부터 잇닿음, D62 4)
	inline constexpr int kStackHipClips = (int)(ACLIP_RECOVERY - ACLIP_JUMP_START);		// 17
	inline constexpr int kStackAdsEnd = kStackHipClips + 12;							// 29
	inline constexpr int kStackClipCount = kStackAdsEnd + 14;							// 43
	inline uint32_t StackClipIndex(int stackClip)
	{
		if (stackClip < kStackHipClips) return (uint32_t)ACLIP_JUMP_START + (uint32_t)stackClip;
		if (stackClip < kStackAdsEnd) return (uint32_t)ACLIP_ADS_TRANSITION_FIRST + (uint32_t)(stackClip - kStackHipClips);
		return (uint32_t)ACLIP_CROUCH_TRANSITION_FIRST + (uint32_t)(stackClip - kStackAdsEnd);
	}
	static_assert(ACLIP_CROUCH_EXIT == ACLIP_CROUCH_TRANSITION_FIRST + 13, "앉은 전환 12 바로 뒤에 앉기 · 일어서기 전환이 있어야 쌓기 칸 번호가 잇닿는다");

	struct BoneKey
	{
		float p[3];
		float q[4];
	};

	// 자세 계산용. 저장은 float, 계산은 double 이다(언리얼 FTransform 과 같다)
	struct BoneTransform
	{
		double p[3];
		double q[4];
	};

	struct ClipBone
	{
		std::string name;
		int parent = -1;
		BoneKey ref{};
	};

	// 판정 부위 하나 — 물리 자산의 몸체. 뼈에 붙어 움직인다. 크기는 BodyShape 와 같은 뜻이다
	struct ClipBody
	{
		int bone = -1;
		uint8_t kind = 0;		// EBodyShape 값(Shared/Weapon.h)
		bool weakSpot = false;
		float center[3] = {};
		float q[4] = {};
		float a = 0;
		float b = 0;
		float c = 0;
	};

	struct ClipMarker
	{
		float time = 0;
		bool left = false;
	};

	struct AnimClip
	{
		std::string name;
		float length = 0;
		uint8_t additive = 0;
		bool loop = false;
		float rootSpeed = 0;
		uint32_t samples = 0;
		std::vector<ClipMarker> markers;
		bool hasCurve = false;
		uint32_t curveHz = 0;
		std::vector<float> curve;
		std::vector<BoneKey> keys;		// 표본 수 × 뼈 수

		const BoneKey* Keys(uint32_t sample, uint32_t boneCount) const { return keys.data() + (size_t)sample * boneCount; }
	};

	// 골격의 뼈별 비중 표 하나. 표에 없는 뼈는 엔진 기본값(뼈 가림 방식 0, 그 밖 1 — BlendProfile.h GetDefaultBlendScale)으로 이미 채워 둔다
	struct ClipProfile
	{
		std::string name;
		uint8_t mode = 0;				// 엔진 EBlendProfileMode 값 그대로(2 = 뼈 가림)
		std::vector<float> weights;		// 남긴 뼈 차례, 뼈 수만큼
	};

	struct ClipSlot
	{
		std::string name;				// 언리얼 슬롯 이름. MOB_ 로 시작하면 엔진 슬롯이 아니라 이 프로젝트가 붙인 자리다
		ESlotMode mode = ESlotMode::Replace;
		int profile = -1;				// ClipProfile 번호, -1 = 모든 뼈 1
	};

	struct MontageTrack
	{
		uint8_t slot = 0;
		uint16_t clip = 0;
		float startPos = 0;				// 이 줄이 몽타주 시각 몇 초에 시작하나
		float clipStart = 0;			// 클립에서 읽는 구간
		float clipEnd = 0;
		float playRate = 1;
	};

	struct MontageCurve
	{
		std::string name;
		int owner = -1;					// -1 = 몽타주 시각 기준, 0 이상 = 그 줄 클립의 시각 기준
		uint32_t hz = 0;
		std::vector<float> values;
	};

	// 몽타주 넣기 · 빼기 곡선으로 옮긴 엔진 EAlphaBlendOption 값의 끝 — 0 = 선형 · 1 = Cubic · 2 = HermiteCubic. 15개 몽타주가 쓰는 셋이다(3-a 덤프, 실측). 더 큰 값은 읽기에서 거부한다
	inline constexpr uint8_t kBlendOptionMax = 2;

	// 엔진 FAlphaBlend::AlphaToBlendOption 의 세 갈래(kBlendOptionMax 까지 — 읽기에서 그 밖은 거부한다).
	// 몽타주 비중(AnimState MontageBlend)과 빼기 비중 표의 뼈별 비중(AnimPose ApplySlot, 엔진 UBlendProfile::CalculateBoneWeight)이 같이 쓴다
	//   0~1 자르기는 std::max(0, std::min(x, 1)) 와 같은 비교 차례로 적었다(머리 파일이라 windows.h 의 max 매크로를 피한다)
	inline float BlendOptionCurve(uint8_t option, float alpha)
	{
		const auto unit = [](float x)
		{
			const float upper = 1.f < x ? 1.f : x;
			return 0.f < upper ? upper : 0.f;
		};
		switch (option)
		{
		case 1:		// Cubic = FMath::CubicInterp(0, 0, 1, 0, a)
		{
			const float a2 = alpha * alpha;
			const float a3 = a2 * alpha;
			return unit((-2.f * a3) + (3.f * a2));
		}
		case 2:		// HermiteCubic = FMath::SmoothStep(0, 1, a)
			if (alpha < 0.f) return 0.f;
			if (alpha >= 1.f) return 1.f;
			return unit(alpha * alpha * (3.f - 2.f * alpha));
		default:	// 선형
			return unit(alpha);
		}
	}

	struct ClipMontage
	{
		std::string name;
		uint8_t kind = 0;				// EMontageKind
		std::string group;				// 같은 무리의 몽타주를 새로 틀면 앞 것을 끈다(엔진 규칙)
		float length = 0;
		float rate = 1;					// 자산 재생 배율(엔진 RateScale). 실제 속도 = 트는 쪽이 준 속도 × 이 값
		float blendIn = 0;
		uint8_t blendInOption = 0;		// 엔진 EAlphaBlendOption 값 그대로(kBlendOptionMax 까지)
		float blendOut = 0;
		uint8_t blendOutOption = 0;
		float blendOutTrigger = -1;
		bool autoBlendOut = true;
		// 빼기 비중 표(버전 6) — ClipProfile 번호, -1 = 없음. 엔진은 빼는 동안 이 표(시간 배율 방식)로 뼈마다 빠르기를 달리한다
		// (FAnimInstanceProxy::SlotEvaluatePoseWithBlendProfiles · UBlendProfile::CalculateBoneWeight, 원본 기준). 대시 넷 = FastFeet(다리 뼈 0.5).
		// 제 빼기(자동 빼기 · StopMontageKind)로 빠질 때만 걸린다 — 상태가 칸마다 들고(MontageInstance::profile) 빼기 진행을 와이어에 실어
		// 자세 계산(AnimPose ApplySlot)이 뼈마다 뺀다(L-6-b · D130. 그 전엔 이 칸을 안 썼다 — content-impl-progress.md 미결 8)
		int16_t blendOutProfile = -1;
		std::vector<MontageTrack> tracks;
		std::vector<MontageCurve> curves;
	};

	struct ClipBundle
	{
		std::string name;
		uint32_t sampleHz = 0;
		float aimPitch[6] = {};
		float groundFriction = 0;
		float brakingFriction = 0;
		float brakingFrictionFactor = 0;
		float brakingDecelerationWalking = 0;
		float gravityZ = 0;
		bool separateBraking = false;
		std::vector<AnimClip> clips;
		std::vector<ClipMontage> montages;
		int overrideClip = -1;			// 쏜 뒤 윗몸 덮기 자세의 클립(시각 0), -1 = 없음
		int overrideSlot = -1;			// 그 자세를 넣는 끼움 자리

		const ClipMontage* FindMontage(uint8_t kind) const
		{
			for (const ClipMontage& montage : montages)
			{
				if (montage.kind == kind) return &montage;
			}
			return nullptr;
		}
	};

	class CAnimClips
	{
	public:
		EClipLoad LoadFromMemory(const void* data, size_t size);
		void Clear();
		bool IsLoaded() const { return !_bundles.empty(); }

		const std::vector<ClipBone>& Bones() const { return _bones; }
		const std::vector<ClipBody>& Bodies() const { return _bodies; }
		const BoneKey& MeshPlacement() const { return _mesh; }
		const std::vector<ClipBundle>& Bundles() const { return _bundles; }
		const std::vector<ClipProfile>& Profiles() const { return _profiles; }
		const std::vector<ClipSlot>& Slots() const { return _slots; }
		int FindBone(const char* name) const;
		int FindSlot(const char* name) const;
		const ClipBundle* FindBundle(const char* name) const;
		size_t KeyBytes() const;		// 키가 차지하는 메모리

		// 한 클립의 time 초 자세를 뼈 수만큼 채운다. 표본 사이는 이동 선형, 회전은 가까운 쪽 부호로 맞춘 뒤 선형 + 정규화(엔진 클립 읽기와 같은 식)
		void Sample(const ClipBundle& bundle, const AnimClip& clip, double time, BoneTransform* out) const;

	private:
		EClipLoad Parse(const void* data, size_t size);

		std::vector<ClipBone> _bones;
		std::vector<ClipBody> _bodies;
		BoneKey _mesh{};
		std::vector<ClipProfile> _profiles;
		std::vector<ClipSlot> _slots;
		std::vector<ClipBundle> _bundles;
	};
}
