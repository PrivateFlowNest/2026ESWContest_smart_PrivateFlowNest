"""작품소개서: fill the contest template (templates/..._작품소개서_양식.docx) and export it with Word.

    python -I build_intro.py      (after `python -I photo.py`, see build.sh)

The template is edited in place (cell by cell) so its table, borders and shading stay exactly as given. Text is
나눔명조 11pt black as the template asks; the blue guide text is removed except the team-photo placeholder, which the
team replaces with the photo. Members and roles: user 2026-10-27 (박정빈 is not in the plan; role from the user).
"""
import html
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TEMPLATE = HERE.parent / "templates" / "2026ESWContest_스마트가전_팀명_작품소개서_양식.docx"
OUT = HERE / "out"
NAME = "2026ESWContest_스마트가전_PrivateFlowNest_작품소개서"
FIGURE = OUT / "model-annotated.png"  # photo.py
FIGURE_W_CM, FIGURE_H_CM = 12.4, 12.4 * 840 / 1480

TEAM = "PrivateFlowNest"
WORK = "HomeMind TaskFlow"
# both links go in the report and the 작품소개서 (세부안내 p5); filled once the repository and the video are up
GITHUB = "github.com/〈사용자이름〉/2026ESWContest_smart_PrivateFlowNest"
YOUTUBE = "〈유튜브 링크: 업로드 후 입력〉"
SSU = "숭실대학교 AI소프트웨어학부"
MEMBERS = [("팀장", "박영만", "가천대학교 기계공학부", "HW·펌웨어"), ("팀원", "안형민", SSU, "AI·UI"),
           ("팀원", "김기원", SSU, "허브 SW·연동"), ("팀원", "박준우", "가천대학교 산업공학전공", "기획·관리"),
           ("팀원", "박정빈", SSU, "모바일 앱")]

OVERVIEW = (
    "말이 끝나기 전에 준비하고, 상황을 보고 먼저 제안하는 온디바이스 AI 스마트홈입니다. "
    "LG AI Native OS가 올라간 Raspberry Pi 5 두 대와 Pi 4, ESP32·ESP8266이 MQTT로 연동되어 "
    "음성 인식, 숨은 의도 이해(EXAONE), 소리 상황 인지를 모두 집 안에서 처리합니다. "
    "AI는 제안만 하고, 실행은 Pi 4의 정책 검증과 사람의 승인을 거칩니다."
)
ENVIRONMENT = [
    ("HW: ", "Raspberry Pi 5 ×2(인식: USB 마이크·웹캠 2대 / UI·LLM: 7인치 터치 디스플레이), Raspberry Pi 4(백엔드), "
             "ESP32(현관문·창문 모터), ESP8266(조명 LED·12V 팬)"),
    ("플랫폼: ", "LG AI Native OS(Pi 세 대), Camera1 D-Bus API, IPK·systemd 서비스"),
    ("SW: ", "C++ 네이티브 서비스, Flutter(7인치 UI·Android 앱), Mosquitto MQTT(인증·ACL), SQLite, Arduino·PlatformIO"),
    ("AI: ", "EXAONE 4.0 1.2B LoRA(llama.cpp), Whisper small LoRA(whisper.cpp), YAMNet·부분 발화 분류(ONNX)"),
    ("개발: ", "Windows PC(RTX 3090) 학습·빌드, Docker arm64 교차 빌드, JSON Schema 메시지 계약(pfn.v1)"),
]
DESCRIPTION = [
    ("① 말 중간 준비: ", "말하는 도중의 문장(부분 발화)으로 대상 기기를 7인치 화면에 미리 표시하고(실행은 안 함), "
                     "말이 끝나면 규칙 파서가 Whisper 문장을 바로 해석합니다."),
    ("② 숨은 의도 이해: ", "“아 좀 덥네”처럼 규칙에 없는 말은 Pi 5의 EXAONE이 ‘에어컨 켜기’를 제안하고, "
                      "7인치 화면이나 휴대폰에서 승인하면 실행합니다."),
    ("③ 상황 인지: ", "소리 상황과 카메라 위치를 합쳐 회의 중임을 알아채면 조용 모드를 자동으로 켜고, "
                  "“상황이 달라요” 한마디로 바로 되돌립니다."),
    ("④ 안전한 실행: ", "Pi 4 core가 기기·출처·정책을 확인하고, 현관문 열기는 집 안 7인치 화면에서만 승인합니다. "
                    "ESP는 중복·만료 명령을 거르고 ACK로 완료를 알립니다."),
    ("⑤ 휴대폰 앱: ", "QR로 연결해 기기 상태 확인과 제어, 승인·거절, 실시간 알림을 받습니다."),
]
FEATURES = [
    ("Edge First: ", "음성 인식·LLM·영상 처리가 모두 집 안 Pi에서 동작해 인터넷이 끊겨도 쓸 수 있고, "
                     "음성·영상 원본은 저장하거나 밖으로 보내지 않습니다."),
    ("규칙 먼저, LLM은 보조: ", "분명한 명령은 규칙으로 바로 처리하고 애매한 말에만 LLM을 써서 빠르고 예측 가능합니다."),
    ("AI는 제안만: ", "최종 실행은 정해진 정책과 사람의 승인으로 결정해, AI의 잘못된 출력이 기기를 직접 움직이지 않습니다."),
    ("확장성: ", "기기·동작·정책을 JSON 계약으로 관리해 새 가전도 같은 구조로 추가할 수 있습니다."),
    ("실기 검증: ", "Pi 세 대에 설치해 음성 인식(3초 발화 → 3.0초), 7인치 화면, 휴대폰 연결, 두 카메라 영상을 확인했습니다."),
]

FONT = '<w:rFonts w:ascii="나눔명조" w:eastAsia="나눔명조" w:hAnsi="나눔명조" w:cs="KoPubWorld바탕체 Medium" w:hint="eastAsia"/>'


def run(value, bold=False, size=22):
    value = value.replace("Pi 5", "Pi 5").replace("Pi 4", "Pi 4")  # keep "Pi 5" on one line
    b = "<w:b/><w:bCs/>" if bold else ""
    return (f'<w:r><w:rPr>{FONT}{b}<w:color w:val="000000"/><w:sz w:val="{size}"/><w:szCs w:val="{size}"/></w:rPr>'
            f'<w:t xml:space="preserve">{html.escape(value, quote=False)}</w:t></w:r>')


def para(runs, jc="left", hanging=0, after=60, line=300, size=22):
    """line is exact (1/20 pt; 0 = single): 나눔명조's own single line is about 1.5 em, too loose for two pages.
    wordWrap 1 makes Word wrap Korean at spaces; without it this template breaks words between any two syllables."""
    ind = f'<w:ind w:left="{hanging}" w:hanging="{hanging}"/>' if hanging else ""
    spacing = f'w:line="{line}" w:lineRule="exact"' if line else 'w:line="240" w:lineRule="auto"'
    return (f'<w:p><w:pPr><w:wordWrap w:val="1"/><w:spacing w:after="{after}" {spacing}/>{ind}'
            f'<w:jc w:val="{jc}"/><w:rPr>{FONT}<w:sz w:val="{size}"/><w:szCs w:val="{size}"/></w:rPr></w:pPr>{"".join(runs)}</w:p>')


def items(entries, mark=None):
    out = []
    for i, (head, body) in enumerate(entries):
        lead = f"{mark} " if mark else ""
        out.append(para([run(lead + head, bold=True), run(body)], hanging=300 if mark else 360,
                        after=80 if i < len(entries) - 1 else 0))
    return "".join(out)


def figure_paragraph(rel_id):
    cx, cy = int(FIGURE_W_CM * 360000), int(FIGURE_H_CM * 360000)
    a = "http://schemas.openxmlformats.org/drawingml/2006/main"
    pic = "http://schemas.openxmlformats.org/drawingml/2006/picture"
    drawing = (
        f'<w:r><w:drawing><wp:inline distT="0" distB="0" distL="0" distR="0"><wp:extent cx="{cx}" cy="{cy}"/>'
        '<wp:effectExtent l="0" t="0" r="0" b="0"/><wp:docPr id="101" name="그림 1" descr="HomeMind TaskFlow 실제 모형 사진"/>'
        f'<wp:cNvGraphicFramePr><a:graphicFrameLocks xmlns:a="{a}" noChangeAspect="1"/></wp:cNvGraphicFramePr>'
        f'<a:graphic xmlns:a="{a}"><a:graphicData uri="{pic}"><pic:pic xmlns:pic="{pic}">'
        '<pic:nvPicPr><pic:cNvPr id="101" name="model-annotated.png"/><pic:cNvPicPr/></pic:nvPicPr>'
        f'<pic:blipFill><a:blip r:embed="{rel_id}"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>'
        f'<pic:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="{cx}" cy="{cy}"/></a:xfrm><a:prstGeom prst="rect"><a:avLst/></a:prstGeom></pic:spPr>'
        '</pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r>'
    )
    return para([drawing], jc="center", after=60, line=0) + para([run("그림 1. HomeMind TaskFlow 실제 모형")], jc="center", after=0)


def set_cell(cell, paragraphs):
    """Keep the cell's properties, replace its content."""
    head = re.match(r"<w:tc>(<w:tcPr>.*?</w:tcPr>)?", cell, flags=re.S).group(0)
    return head + paragraphs + "</w:tc>"


def fill(document, rel_id):
    cells = list(re.finditer(r"<w:tc>.*?</w:tc>", document, flags=re.S))
    assert len(cells) == 44, f"template changed: {len(cells)} cells"
    texts = [re.sub(r"<[^>]+>", "", c.group(0)) for c in cells]
    assert texts[1].replace(" ", "") == "팀명" and texts[38].replace(" ", "") == "개발환경", "template layout changed"

    new = {
        2: para([run(TEAM)], jc="center", after=0),
        4: para([run(WORK)], jc="center", after=0),
        35: figure_paragraph(rel_id),
        37: para([run(OVERVIEW)], after=60)
            + para([run("소스코드: ", bold=True), run(GITHUB)], after=0) + para([run("시연 영상: ", bold=True), run(YOUTUBE)], after=0),
        39: items(ENVIRONMENT, mark="○"),
        41: items(DESCRIPTION),
        43: items(FEATURES, mark="○"),
    }
    # member rows: [team-photo continuation, 구분, 성명, 소속, 업무분장]
    for row, (kind, name, school, role) in enumerate(MEMBERS):
        new[11 + 5 * row] = para([run(kind)], jc="center", after=0)
        new[12 + 5 * row] = para([run(name)], jc="center", after=0)
        new[13 + 5 * row] = para([run(school)], jc="center", after=0, line=280)  # two lines; tighter so page 1 keeps the overview
        new[14 + 5 * row] = para([run(role)], jc="center", after=0)
    pieces, last = [], 0
    for index, match in enumerate(cells):
        pieces.append(document[last:match.start()])
        cell = match.group(0)
        if index in new:
            cell = set_cell(cell, new[index])
        elif index == 0:  # title cell: "[부문명]" (split over several runs) -> "[스마트 가전]" in black
            cell = re.sub(r"<w:p [^>]*>.*?</w:p>",
                          lambda p: para([run("[스마트 가전]", size=32)], jc="center", after=0, size=32)
                          if "부문명" in re.sub(r"<[^>]+>", "", p.group(0)) else p.group(0), cell, flags=re.S)
        elif index != 5:  # everything except the team-photo placeholder turns black
            cell = cell.replace('w:val="0000FF"', 'w:val="000000"')
        assert index == 5 or "0000FF" not in cell, f"blue text left in cell {index}"
        pieces.append(cell)
        last = match.end()
    pieces.append(document[last:])
    document = "".join(pieces)

    # the blue notes under the table go; Word needs one paragraph after a table, so keep a 1-pt one
    tail = re.search(r"</w:tbl>(.*?)(<w:sectPr)", document, flags=re.S)
    tiny = '<w:p><w:pPr><w:spacing w:after="0" w:line="20" w:lineRule="exact"/><w:rPr><w:sz w:val="2"/></w:rPr></w:pPr></w:p>'
    document = document[:tail.start(1)] + tiny + document[tail.end(1):]
    return document


def main():
    if not FIGURE.exists():
        sys.exit("out/model-annotated.png missing: run build.sh")
    OUT.mkdir(exist_ok=True)
    docx = OUT / f"{NAME}.docx"
    rel_id = "rIdHomeMindFigure1"
    with zipfile.ZipFile(TEMPLATE) as src, zipfile.ZipFile(docx, "w", zipfile.ZIP_DEFLATED) as dst:
        for item in src.infolist():
            data = src.read(item.filename)
            if item.filename == "word/document.xml":
                data = fill(data.decode("utf-8"), rel_id).encode("utf-8")
            elif item.filename == "word/_rels/document.xml.rels":
                rel = (f'<Relationship Id="{rel_id}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" '
                       'Target="media/homemind-figure.png"/>')
                data = data.decode("utf-8").replace("</Relationships>", rel + "</Relationships>").encode("utf-8")
            elif item.filename == "[Content_Types].xml":
                text = data.decode("utf-8")
                if 'Extension="png"' not in text:
                    text = text.replace("<Default Extension=\"xml\"", '<Default Extension="png" ContentType="image/png"/><Default Extension="xml"', 1)
                data = text.encode("utf-8")
            dst.writestr(item, data)
        dst.write(FIGURE, "word/media/homemind-figure.png")
    shutil.copyfile(HERE / "assets" / "model-photo-wide.jpg", OUT / f"{NAME}_작품사진.jpg")  # the original photo, attached separately as asked
    result = subprocess.run(["powershell", "-ExecutionPolicy", "Bypass", "-File", str(HERE / "render.ps1"), str(docx)],
                            capture_output=True, text=True, check=True)
    print(result.stdout.strip())


if __name__ == "__main__":
    main()
