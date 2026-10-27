HomeMind 공유용 소스

이 폴더를 Flutter 프로젝트 루트로 열어 주세요.
- lib/: Flutter/Dart 앱 코드
- android/: Android 실행 및 빌드 설정
- backend/: Python/FastAPI 서버와 MQTT 연결 코드
- assets/: 앱 아이콘 이미지
- test/: Flutter 테스트

앱 실행 준비
1. Flutter SDK와 Android SDK를 설치합니다. pubspec.yaml의 Dart SDK 조건은 ^3.13.2입니다.
2. 이 폴더에서 flutter pub get을 실행합니다.
3. Android 기기 또는 에뮬레이터를 연결하고 flutter run을 실행합니다.
4. 개인 PC 경로가 들어가는 android/local.properties는 포함하지 않았습니다. Flutter/Android Studio에서 로컬 SDK 경로를 설정해 주세요.

서버 실행 준비
1. backend 폴더에서 Python 가상환경을 만들고 활성화합니다.
2. pip install -r requirements-dev.txt
3. python -m uvicorn app.main:app --host 0.0.0.0 --port 8000
4. MQTT를 사용할 때는 backend/.env.example의 항목을 참고해 환경 변수를 설정합니다. 서버가 .env 파일을 자동으로 읽지는 않습니다.

현재 구현 범위
- QR 스캔, 내용 검사, 사용자 승인 화면까지 구현되어 있습니다.
- (2026-10-22) 페어링 claim API와 토큰 인증을 연결했습니다: QR 승인 → POST /api/v1/pairing/claim → 토큰을 보안 저장소에 두고 모든 요청에 Bearer로 보냅니다. 서버 쪽은 backend/README.md 참고.
- 데모 모드와 서버 주소 직접 연결은 디버그 모드에서 표시됩니다.
- 에뮬레이터의 기본 서버 주소는 http://10.0.2.2:8000 입니다.
- 실제 장치 명령에는 MQTT 연결이 필요합니다.
- 원본 README.md와 backend/README.md도 참고해 주세요.

공유 범위
앱·서버 소스, 리소스, 의존성 및 빌드 설정, 테스트와 문서를 포함했습니다.
빌드 산출물, 캐시, IDE 개인 설정, 가상환경, 로컬 SDK 경로 및 실제 비밀정보 파일은 제외했습니다.
이 안내문만 공유본에 추가했으며 원본 프로젝트 파일은 변경하지 않았습니다.
공유본 생성 시 새 빌드 또는 실행 검증은 수행하지 않았습니다.