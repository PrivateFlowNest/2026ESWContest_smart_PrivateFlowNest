# Cut the real footage (footage/raw/, phone recordings of the model) into the video's footage clips.
#   python footage.py            -> footage/<clip>.mp4 (1280x720, 30 fps, sound kept unless muted)
# Each clip is a list of pieces taken from the raw recordings. Between two pieces of the same take, the wait is
# cut and the video says so ("N초 생략"); results are never taken from a different take than the command.
# HLG (HDR) recordings are tone-mapped to SDR. Overlays (brand, "실기 촬영", captions) are drawn with PIL.
import os
import subprocess
import tempfile

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.abspath(__file__))
FF = os.path.join(ROOT, 'node_modules', 'ffmpeg-static', 'ffmpeg.exe')
RAW = os.path.join(ROOT, 'footage', 'raw')
OUT = os.path.join(ROOT, 'footage')
FONT = os.path.join(ROOT, '..', '..', '..', 'team', 'homemind-core-ui-2026-10-20', 'homemind_flutter_test',
                    'assets', 'fonts', 'NotoSansKR-VariableFont_wght.ttf')
W, H, FPS = 1280, 720, 30
TONEMAP = ('zscale=t=linear:npl=100,format=gbrpf32le,zscale=p=bt709,tonemap=tonemap=hable:desat=0,'
           'zscale=t=bt709:m=bt709:r=tv,format=yuv420p')
FIT = f'scale={W}:{H}:force_original_aspect_ratio=decrease,pad={W}:{H}:(ow-iw)/2:(oh-ih)/2:color=0x0A1C24'

TAKE = {
    'tour': '2026_10_27_21_32_(4).mp4',      # whole model, portrait
    'smell': '2026_10_27_21_32_(2).mp4',     # 거실 너무 밝아 / 창문 열어·닫아 / 화장실 냄새 / 화장실 사용 끝
    'voice': '2026_10_27_21_32_(3).mp4',     # 거실 불 켜줘 / 에어컨 켜줘 / 좀 추운데 ...
    'screen': '2026_10_27_21_50.mp4',        # the real 7-inch screen, camera page
}
HDR = {'tour', 'smell', 'voice'}

# piece: (take, from, to, caption, sub, options); 'cut' marks that the wait before this piece was cut
CLIPS = {
    'model-tour': ('실제 모형', [
        ('tour', 0.3, 4.9, '실제로 만든 스마트홈 모형', 'Pi 5 두 대 · Pi 4 · ESP32 · ESP8266 · 창문 · LED · 환기팬', {'mute': True}),
    ]),
    'hidden-intent': ('숨은 의도 이해 · 실기', [
        ('smell', 50.4, 53.6, '“화장실 냄새가 좀 나는 것 같아…”', '명령이 아닌 혼잣말', {}),
        ('smell', 76.0, 84.0, '환기팬이 켜집니다', 'EXAONE이 말 속의 의도(환기)를 해석', {'cut': 'EXAONE 처리', 'inset': (1170, 395, 180)}),
        ('voice', 20.7, 23.3, '“좀 추운데…”', '', {}),
        ('voice', 50.2, 54.0, '에어컨 표시등이 꺼집니다', '‘에어컨 꺼짐’으로 해석', {'cut': 'EXAONE 처리'}),
    ]),
    'window-door': ('바로 실행 · 실기', [
        ('smell', 29.4, 31.4, '“창문 열어 줘”', '분명한 명령은 규칙 파서가 바로 처리', {}),
        ('smell', 36.6, 39.2, '창문이 열립니다', 'ESP32 · 열기/닫기 모터', {'cut': ''}),
        ('smell', 40.1, 42.0, '“창문 닫아 줘”', '', {}),
        ('smell', 45.6, 47.8, '창문이 닫힙니다', '', {'cut': ''}),
    ]),
    'partial-hint': ('음성 명령 · 실기', [
        ('voice', 1.5, 3.3, '“거실 불 켜 줘”', '', {}),
        ('voice', 9.3, 11.3, '거실 조명이 켜집니다', 'ESP8266 · LED', {'cut': ''}),
        ('voice', 12.2, 13.9, '“에어컨 켜 줘”', '', {}),
        ('voice', 16.8, 19.0, '에어컨 표시등이 켜집니다', '', {'cut': ''}),
    ]),
    'camera-page': ('Edge First · 실기', [
        ('screen', 0.0, 5.0, '실기 7인치 화면 · 카메라', '카메라 영상은 집 안 공유기로만 · 저장하지 않음', {'mute': True, 'nobrand': True}),
    ]),
}


def font(size, weight):
    f = ImageFont.truetype(FONT, size)
    f.set_variation_by_axes([weight])
    return f


def pill(d, xy, text, f, fill, outline=None, color='white', pad=(12, 5), radius=999):
    x, y = xy
    l, t, r, b = d.textbbox((0, 0), text, font=f)
    box = (x, y, x + r - l + pad[0] * 2, y + b - t + pad[1] * 2)
    d.rounded_rectangle(box, radius=min(radius, (box[3] - box[1]) // 2), fill=fill, outline=outline, width=1)
    d.text((x + pad[0] - l, y + pad[1] - t), text, font=f, fill=color)
    return box


def overlay_png(path, chapter, caption, sub, inset_label=False, brand=True):
    im = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    if brand:  # off where the footage has its own screen text in that corner
        d.ellipse((28, 27, 38, 37), fill=(232, 116, 59, 255))
        d.text((46, 21), 'HomeMind TaskFlow', font=font(17, 700), fill=(234, 246, 248, 255))
        pill(d, (28, 50), chapter, font(14, 700), (232, 116, 59, 255))
    kind = font(13, 500)
    tw = d.textlength('실기 촬영 · 실제 모형', font=kind)
    pill(d, (W - 26 - tw - 22, 22), '실기 촬영 · 실제 모형', kind, (8, 40, 24, 200), outline=(155, 231, 166, 255),
         color=(220, 248, 225, 255), pad=(11, 5), radius=6)
    if inset_label:
        pill(d, (880, 450), '환기팬 확대', font(14, 700), (8, 26, 33, 220), radius=6)
    if caption:
        fm, fs = font(25, 700), font(16, 500)
        wm = d.textlength(caption, font=fm)
        ws = d.textlength(sub, font=fs) if sub else 0
        bw = max(wm, ws) + 44
        bh = 52 + (24 if sub else 0)
        x0, y1 = (W - bw) / 2, H - 34
        d.rounded_rectangle((x0, y1 - bh, x0 + bw, y1), radius=12, fill=(5, 18, 23, 220))
        d.text(((W - wm) / 2, y1 - bh + 10), caption, font=fm, fill='white')
        if sub:
            d.text(((W - ws) / 2, y1 - bh + 48), sub, font=fs, fill=(169, 205, 211, 255))
    im.save(path)


def cut_png(path, text):
    im = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    f = font(19, 700)
    tw = d.textlength(text, font=f)
    pill(d, ((W - tw - 36) / 2, H / 2 - 24), text, f, (60, 44, 120, 235), outline=(200, 182, 255, 255), pad=(18, 9))
    im.save(path)


def piece(tmp, i, chapter, prev_end, spec):
    take, t0, t1, caption, sub, opt = spec
    dur = t1 - t0
    src = os.path.join(RAW, TAKE[take])
    ov = os.path.join(tmp, f'ov{i}.png')
    overlay_png(ov, chapter, caption, sub, inset_label='inset' in opt, brand=not opt.get('nobrand'))
    inputs = ['-ss', f'{t0:.3f}', '-t', f'{dur:.3f}', '-i', src, '-loop', '1', '-t', f'{dur:.3f}', '-i', ov]
    tm = TONEMAP + ',' if take in HDR else ''
    graph = [f'[0:v]{tm}split[a][b]', f'[a]{FIT},fps={FPS}[bg]']
    last = 'bg'
    if 'inset' in opt:
        x, y, s = opt['inset']
        graph.append(f'[b]crop={s}:{s}:{x}:{y},scale=330:330,pad=336:336:3:3:color=white[ins]')
        graph.append(f'[{last}][ins]overlay=880:110[v1]')
        last = 'v1'
    else:
        graph.append('[b]nullsink')
    graph.append(f'[{last}][1:v]overlay=0:0[v2]')
    last = 'v2'
    if 'cut' in opt and prev_end is not None:
        skipped = round(t0 - prev_end)
        label = f'⋯  {skipped}초 생략' + (f' · {opt["cut"]}' if opt['cut'] else '') + '  ⋯'
        cp = os.path.join(tmp, f'cut{i}.png')
        cut_png(cp, label)
        inputs += ['-loop', '1', '-t', f'{dur:.3f}', '-i', cp]
        graph.append(f"[{last}][2:v]overlay=0:0:enable='lt(t,1.6)'[v3]")
        last = 'v3'
    graph.append(f'[{last}]format=yuv420p,setsar=1[v]')
    if opt.get('mute'):
        inputs += ['-f', 'lavfi', '-t', f'{dur:.3f}', '-i', 'anullsrc=channel_layout=stereo:sample_rate=48000']
        graph.append(f'[{audio_index(inputs)}:a]anull[aout]')
    else:
        fade = max(0.0, dur - 0.06)
        graph.append(f'[0:a]aresample=48000,aformat=channel_layouts=stereo,afade=t=in:d=0.06,afade=t=out:st={fade:.3f}:d=0.06[aout]')
    out = os.path.join(tmp, f'p{i}.mp4')
    run = subprocess.run([FF, '-y', '-loglevel', 'error', *inputs, '-filter_complex', ';'.join(graph),
                          '-map', '[v]', '-map', '[aout]', '-r', str(FPS), '-c:v', 'libx264', '-preset', 'medium',
                          '-crf', '17', '-c:a', 'aac', '-b:a', '160k', '-ar', '48000', '-shortest', out])
    if run.returncode:
        raise SystemExit(f'ffmpeg failed on piece {i} ({take} {t0}-{t1})')
    return out


def audio_index(inputs):
    # index of the last '-i' input (the silent source)
    return sum(1 for a in inputs if a == '-i') - 1


def main():
    for name, (chapter, pieces) in CLIPS.items():
        with tempfile.TemporaryDirectory() as tmp:
            parts, prev = [], None
            for i, spec in enumerate(pieces):
                same_take = prev is not None and prev[0] == spec[0]
                parts.append(piece(tmp, i, chapter, prev[1] if same_take else None, spec))
                prev = (spec[0], spec[2])
            lst = os.path.join(tmp, 'list.txt')
            with open(lst, 'w', encoding='utf-8') as f:
                f.writelines(f"file '{p}'\n" for p in parts)
            out = os.path.join(OUT, f'{name}.mp4')
            subprocess.run([FF, '-y', '-loglevel', 'error', '-f', 'concat', '-safe', '0', '-i', lst, '-c', 'copy',
                            '-movflags', '+faststart', out], check=True)
            total = sum(p[2] - p[1] for p in pieces)
            print(f'{name}: {len(pieces)} pieces, {total:.1f}s -> footage/{name}.mp4')


if __name__ == '__main__':
    main()
