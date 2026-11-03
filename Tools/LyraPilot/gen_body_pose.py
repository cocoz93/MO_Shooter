# Writes Shared/BodyPose.inl from the SHAPE lines of a weapon record.
# usage: python gen_body_pose.py <weaponrec.csv> <BodyPose.inl>
import sys

rows = []
for line in open(sys.argv[1], encoding="utf-8"):
    c = line.strip().split(",")
    if c[0] != "SHAPE":
        continue
    rows.append(c)

kinds = {"sphere": "EBodyShape::Sphere", "sphyl": "EBodyShape::Sphyl", "box": "EBodyShape::Box"}
out = []
out.append("// 자동 생성 — Tools/LyraPilot/gen_body_pose.py. 손으로 고치지 말 것.")
out.append("// 원본: Lyra 봇이 서 있는 포즈의 물리 자산 몸체(PA_Mannequin). 캡슐 중심 기준, 요 0.")
out.append("const BodyShape kBodyShapes[] = {")


def f(s):
    return s + "f"


for c in rows:
    bone, kind, weak = c[1], c[2], c[3]
    nums = [f(x) for x in c[4:]]
    center, ax, ay, az, a, b, cc = nums[0:3], nums[3:6], nums[6:9], nums[9:12], nums[12], nums[13], nums[14]
    out.append(
        '\t{ "%s", %s, %s, { %s }, { %s }, { %s }, { %s }, %s, %s, %s },'
        % (bone, kinds[kind], "true" if weak == "1" else "false", ", ".join(center), ", ".join(ax), ", ".join(ay), ", ".join(az), a, b, cc)
    )
out.append("};")
out.append("")
text = "\r\n".join(out)
with open(sys.argv[2], "w", encoding="utf-8-sig", newline="") as fp:
    fp.write(text)
print("shapes", len(rows))
