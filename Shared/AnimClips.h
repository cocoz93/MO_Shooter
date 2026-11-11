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
//   u32 묶음 수, 묶음마다 {
//     u8 이름 길이, 이름, u32 표본 Hz, 6f 조준 각도(위 · 가운데 · 아래 · 느슨한 위 · 가운데 · 아래),
//     5f (groundFriction, brakingFriction, brakingFrictionFactor, brakingDecelerationWalking, gravityZ), u8 separateBraking,
//     u32 클립 수, 클립마다 {
//       u8 이름 길이, 이름, f 길이(초), u8 덧붙임 종류(0 = 일반), u8 반복, f rootSpeed, u32 표본 수,
//       u16 발 표시 수, 표시마다 { f 시각, u8 왼발 },
//       u8 곡선 유무, 있으면 { u32 hz, u32 값 수, f[값 수] },
//       키 = 표본 수 × 뼈 수 × 7f (px py pz qx qy qz qw) } }
//   'MOAE'
// 크기 칸은 넣지 않는다. 일반 클립은 1, 덧붙임 클립은 0 으로 항상 같고, 다르면 도구가 파일을 만들지 않는다.
namespace mo
{
	inline constexpr uint32_t kClipPackVersion = 1;

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
		ACLIP_COUNT,										// 30
	};

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
		int FindBone(const char* name) const;
		const ClipBundle* FindBundle(const char* name) const;
		size_t KeyBytes() const;		// 키가 차지하는 메모리

		// 한 클립의 time 초 자세를 뼈 수만큼 채운다. 표본 사이는 이동 선형, 회전은 가까운 쪽 부호로 맞춘 뒤 선형 + 정규화(엔진 클립 읽기와 같은 식)
		void Sample(const ClipBundle& bundle, const AnimClip& clip, double time, BoneTransform* out) const;

	private:
		EClipLoad Parse(const void* data, size_t size);

		std::vector<ClipBone> _bones;
		std::vector<ClipBody> _bodies;
		BoneKey _mesh{};
		std::vector<ClipBundle> _bundles;
	};
}
