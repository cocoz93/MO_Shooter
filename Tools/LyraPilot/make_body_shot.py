# record_body_shot.ps1 이 찍은 원시 그림 넉 장(앞 · 앞+판정 · 옆 · 옆+판정)을 한 장으로 합치고, 로그의 MOB_BODYSHOT body 줄 값을 표로 붙인다.
#   python make_body_shot.py <Saved\MOB 폴더> <tag> <가로> <세로> <출력 png>
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

out_dir, tag, w, h, png = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
SHOTS = ['front', 'front_shapes', 'side', 'side_shapes']
FONT = 'C:/Windows/Fonts/malgun.ttf'
FONT_B = 'C:/Windows/Fonts/malgunbd.ttf'

tiles = []
for n, name in enumerate(SHOTS):
    path = os.path.join(out_dir, 'bodyshot_%s_%d_%s.bgra' % (tag, n, name))
    data = open(path, 'rb').read()
    if len(data) != w * h * 4:
        sys.exit('크기가 다름: %s %d != %d' % (path, len(data), w * h * 4))
    tiles.append(Image.frombytes('RGBA', (w, h), data, 'raw', 'BGRA').convert('RGB'))

bodies = {}
for line in open(os.path.join(out_dir, 'bodyshot_%s.log' % tag), encoding='utf-8', errors='replace'):
    m = re.search(r'MOB_BODYSHOT body i=(\d) name=(\w+) meshes=\[([^\]]*)\] physicsAsset=(\S+) shapes=(\d+) weakBodies=(\d+) weakBones=\[([^\]]*)\] '
                  r'headBone=([\d.-]+) shapeTop=([\d.-]+) headRadius=([\d.-]+) headLength=([\d.-]+) shoulder=([\d.-]+) pelvis=([\d.-]+)', line)
    if m:
        g = m.groups()
        bodies[int(g[0])] = dict(name=g[1], meshes=g[2].strip(), pa=g[3], shapes=int(g[4]), weak=g[6].strip(),
                                 head=float(g[7]), top=float(g[8]), hr=float(g[9]), hl=float(g[10]), shoulder=float(g[11]), pelvis=float(g[12]))
if len(bodies) != 2:
    sys.exit('MOB_BODYSHOT body 줄이 %d개' % len(bodies))

TW, TH = 800, 600
PAD = 16
LABEL = 44
TOP = 120
ROWS_TABLE = 7
TABLE_H = 46 + ROWS_TABLE * 34 + 50
W = PAD + LABEL + 2 * TW + PAD * 2
H = TOP + 40 + 2 * TH + PAD * 2 + TABLE_H
img = Image.new('RGB', (W, H), (24, 26, 30))
d = ImageDraw.Draw(img)
f_title = ImageFont.truetype(FONT_B, 34)
f_sub = ImageFont.truetype(FONT, 20)
f_head = ImageFont.truetype(FONT_B, 24)
f_cell = ImageFont.truetype(FONT, 21)
f_cell_b = ImageFont.truetype(FONT_B, 21)

d.text((PAD, 18), 'Lyra 기본 몸 둘 — 왼쪽 Manny · 오른쪽 Quinn', font=f_title, fill=(240, 240, 240))
d.text((PAD, 70), '같은 자리 · 같은 거리(6.5m) · 같은 무기(소총). 원근 때문에 키가 달라 보이지 않게 시야를 30도로 좁혔다. '
                  '아래 줄은 판정 모양(빨강 = 머리 약점, 하늘색 = 몸)', font=f_sub, fill=(170, 175, 185))

x0 = PAD + LABEL
y0 = TOP + 40
for c, title in enumerate(['앞', '옆']):
    d.text((x0 + c * (TW + PAD) + TW // 2 - 12, TOP + 4), title, font=f_head, fill=(230, 230, 230))
for r, title in enumerate(['겉모습', '판정']):
    y = y0 + r * (TH + PAD) + TH // 2
    for k, ch in enumerate(title):
        d.text((PAD + 6, y - len(title) * 15 + k * 30), ch, font=f_head, fill=(230, 230, 230))
for n, t in enumerate(tiles):
    c, r = (0 if n < 2 else 1), n % 2
    img.paste(t.resize((TW, TH), Image.LANCZOS), (x0 + c * (TW + PAD), y0 + r * (TH + PAD)))

ty = y0 + 2 * (TH + PAD) + 10
cols = [PAD, PAD + 330, PAD + 650, PAD + 970]
d.text((cols[0], ty), '값(cm, 발바닥 기준)', font=f_cell_b, fill=(200, 200, 200))
d.text((cols[1], ty), 'Manny', font=f_cell_b, fill=(200, 200, 200))
d.text((cols[2], ty), 'Quinn', font=f_cell_b, fill=(200, 200, 200))
d.text((cols[3], ty), '차이(Quinn - Manny)', font=f_cell_b, fill=(200, 200, 200))
a, b = bodies[0], bodies[1]
rows = [
    ('판정 맨 위(키)', a['top'], b['top']),
    ('머리 뼈 높이', a['head'], b['head']),
    ('골반 뼈 높이', a['pelvis'], b['pelvis']),
    ('어깨 폭(양 어깨 뼈 사이)', a['shoulder'], b['shoulder']),
    ('머리 판정 반지름', a['hr'], b['hr']),
    ('판정 모양 수', a['shapes'], b['shapes']),
]
for k, (name, va, vb) in enumerate(rows):
    y = ty + 40 + k * 34
    fmt = (lambda v: '%d' % v) if isinstance(va, int) else (lambda v: '%.1f' % v)
    d.text((cols[0], y), name, font=f_cell, fill=(225, 225, 225))
    d.text((cols[1], y), fmt(va), font=f_cell, fill=(225, 225, 225))
    d.text((cols[2], y), fmt(vb), font=f_cell, fill=(225, 225, 225))
    diff = vb - va
    d.text((cols[3], y), ('%+d' % diff) if isinstance(va, int) else ('%+.1f' % diff), font=f_cell, fill=(255, 200, 120) if abs(diff) >= 0.5 else (160, 165, 175))
y = ty + 40 + len(rows) * 34 + 6
d.text((cols[0], y), '몸 자산: Manny = %s / Quinn = %s · 판정 자산: %s / %s' % (a['meshes'], b['meshes'], a['pa'], b['pa']), font=f_sub, fill=(150, 155, 165))
d.text((cols[0], y + 28), '머리 약점 부위: Manny = %s / Quinn = %s' % (a['weak'], b['weak']), font=f_sub, fill=(150, 155, 165))

img.save(png)
print('saved', png, img.size)
for i in (0, 1):
    print(bodies[i])
