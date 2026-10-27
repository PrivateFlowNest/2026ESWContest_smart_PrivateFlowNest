import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_application/app/homemind_app.dart';
import 'package:homemind_application/core/api/homemind_api.dart';
import 'package:homemind_application/models/approval_models.dart';
import 'package:homemind_application/models/hardware_models.dart';
import 'package:homemind_application/state/app_controller.dart';

ApprovalItem _item(String id, {bool door = false}) => ApprovalItem(
  taskId: id,
  title: door ? '현관문 열기' : '에어컨 표시등 켜기',
  intent: 'device_control',
  targetDevice: door ? 'SERVO-DOOR-01' : 'LED-AIRCON-01',
  origin: door ? 'voice_parser' : 'llm',
  phoneCanApprove: !door,
  waitingSinceMs: DateTime.now().millisecondsSinceEpoch,
  note: door ? '현관문 열기는 집 안 7인치 화면에서 승인해요' : null,
);

/// Mobile API 0.5 stand-in: approvals list, approve / reject, live events.
class _FakeApi extends HomeMindApi {
  _FakeApi() {
    setToken('test-token');
  }

  List<ApprovalItem> waiting = [];
  final decisions = <String>[];
  int dashboardCalls = 0;
  int approvalCalls = 0;
  final events = StreamController<LiveEvent>();
  HomeMindApiException? decideError;

  @override
  Future<HardwareSnapshot> getDashboard() async {
    dashboardCalls++;
    return HardwareSnapshot(
      connected: true,
      devices: HardwareDemoData.snapshot.devices,
      updatedAt: 'now',
    );
  }

  @override
  Future<List<ApprovalItem>> getApprovals() async {
    approvalCalls++;
    return List.of(waiting);
  }

  @override
  Future<void> decideTask(String taskId, {required bool approve}) async {
    if (decideError != null) throw decideError!;
    decisions.add('${approve ? 'approve' : 'reject'} $taskId');
    waiting.removeWhere((item) => item.taskId == taskId);
  }

  @override
  Stream<LiveEvent> liveEvents({
    int? after,
    Duration silence = const Duration(seconds: 40),
  }) => events.stream;
}

Future<void> _tick() => Future<void>.delayed(const Duration(milliseconds: 20));

void main() {
  test('server-sent events: comments skipped, multi-line data, bad JSON ignored', () async {
    final lines = Stream.fromIterable([
      ': keep-alive',
      '',
      'id: 1',
      'event: task',
      'data: {"seq":1,"task_id":"a","state":"proposed"}',
      '',
      'event: approvals',
      'data: {"seq":2,',
      'data: "pending":1}',
      '',
      'event: task',
      'data: not json',
      '',
      'data: {"seq":3}',
      '',
    ]);
    final events = await parseServerSentEvents(lines).toList();
    expect(events.map((e) => e.type), ['task', 'approvals', 'message']);
    expect(events[0].data['state'], 'proposed');
    expect(events[1].data['pending'], 1);
    expect(events.map((e) => e.seq), [1, 2, 3]);
  });

  test('new requests raise an alert; approve and reject leave the list', () async {
    final api = _FakeApi()..waiting = [_item('t-llm'), _item('t-door', door: true)];
    final controller = AppController(api: api);
    await controller.refresh();
    await controller.refreshApprovals();
    expect(controller.approvals.length, 2);
    expect(controller.approvalAlert, '승인 요청 2건');
    controller.selectTab(AppController.approvalsTab);
    expect(controller.approvalAlert, isNull);

    expect(await controller.decideApproval(controller.approvals.first, approve: true), isTrue);
    expect(api.decisions, ['approve t-llm']);
    expect(controller.lastMessage, contains('승인했습니다'));
    expect(controller.approvals.single.taskId, 't-door');

    api.decideError = const HomeMindApiException('현관문 열기는 집 안 7인치 화면에서 승인해요.');
    expect(await controller.decideApproval(controller.approvals.single, approve: true), isFalse);
    expect(controller.lastError, contains('7인치'));
    api.decideError = null;
    expect(await controller.decideApproval(controller.approvals.single, approve: false), isTrue);
    expect(api.decisions.last, 'reject t-door');
    expect(controller.lastMessage, '거절했습니다.');
    expect(controller.approvals, isEmpty);
    controller.dispose();
  });

  test('live events: open, approvals and finished tasks refresh at once', () async {
    final api = _FakeApi();
    final controller = AppController(api: api);
    await controller.refresh();
    controller.selectTab(0);
    controller.startLiveEvents();
    api.events.add(const LiveEvent('open', {}));
    await _tick();
    expect(controller.liveConnected, isTrue);
    final approvalCallsAfterOpen = api.approvalCalls;

    api.waiting = [_item('t-new')];
    api.events.add(const LiveEvent('approvals', {'seq': 1, 'pending': 1}));
    await _tick();
    expect(api.approvalCalls, approvalCallsAfterOpen + 1);
    expect(controller.approvalAlert, '승인 요청: 에어컨 표시등 켜기');

    final dashboards = api.dashboardCalls;
    api.events.add(const LiveEvent('task', {'seq': 2, 'state': 'running'}));
    await _tick();
    expect(api.dashboardCalls, dashboards);
    api.events.add(const LiveEvent('task', {'seq': 3, 'state': 'completed'}));
    await _tick();
    expect(api.dashboardCalls, dashboards + 1);

    api.events.addError(const HomeMindApiException('lost'));
    await _tick();
    expect(controller.liveConnected, isFalse);
    controller.dispose();
  });

  test('HTTP client against a local server: list, decide, 403, events, 401', () async {
    // flutter_test answers every HttpClient request with 400 once a widget test binding exists
    final mocked = HttpOverrides.current;
    HttpOverrides.global = null;
    addTearDown(() => HttpOverrides.global = mocked);
    final server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    final seen = <String>[];
    server.listen((request) async {
      seen.add('${request.method} ${request.uri} ${request.headers.value('authorization')}');
      final response = request.response;
      if (request.headers.value('authorization') != 'Bearer good') {
        response.statusCode = 401;
        response.write('{"detail":"missing or invalid token"}');
      } else if (request.uri.path == '/api/v1/approvals') {
        response.headers.contentType = ContentType.json;
        response.write(jsonEncode({
          'approvals': [
            {
              'task_id': 'd1',
              'title': '현관문 열기',
              'intent': 'device_control',
              'target_device': 'SERVO-DOOR-01',
              'origin': 'voice_parser',
              'phone_can_approve': false,
              'note': '현관문 열기는 집 안 7인치 화면에서 승인해요',
              'waiting_since_ms': 5,
            },
          ],
        }));
      } else if (request.uri.path == '/api/v1/tasks/d1/approve') {
        response.statusCode = 403;
        response.write('{"detail":"door"}');
      } else if (request.uri.path == '/api/v1/tasks/d1/reject') {
        response.statusCode = 202;
        response.write('{"task_id":"d1","action":"reject","status":"sent"}');
      } else if (request.uri.path == '/api/v1/events') {
        response.headers.set('Content-Type', 'text/event-stream');
        response.bufferOutput = false;
        response.write(': keep-alive\n\n');
        await response.flush();
        response.write('id: 7\nevent: approvals\ndata: {"type":"approvals","pending":0,"seq":7}\n\n');
        await response.flush();
      } else {
        response.statusCode = 404;
      }
      await response.close();
    });
    final api = HomeMindApi(baseUrl: 'http://127.0.0.1:${server.port}');
    api.setToken('good');
    final list = await api.getApprovals();
    expect(list.single.phoneCanApprove, isFalse);
    expect(list.single.originLabel, '음성 명령');
    expect(list.single.note, contains('7인치'));
    await expectLater(
      api.decideTask('d1', approve: true),
      throwsA(isA<HomeMindApiException>().having((e) => e.message, 'message', contains('7인치'))),
    );
    await api.decideTask('d1', approve: false);
    final events = await api.liveEvents(after: 3).toList();
    expect(events.map((e) => e.type), ['open', 'approvals']);
    expect(events.last.seq, 7);
    expect(seen.last, contains('/api/v1/events?after=3'));

    api.setToken('revoked');
    await expectLater(
      api.liveEvents().toList(),
      throwsA(isA<HomeMindUnauthorizedException>()),
    );
    api.close();
    await server.close(force: true);
  });

  testWidgets('demo: approvals tab, door approval only in the house', (tester) async {
    await tester.pumpWidget(
      const HomeMindApp(autoConnect: false, initialMode: HomeMindEntryMode.demo),
    );
    await tester.pumpAndSettle();
    expect(find.text('승인'), findsOneWidget);
    expect(find.text('2'), findsOneWidget); // badge

    await tester.tap(find.text('승인'));
    await tester.pumpAndSettle();
    expect(find.text('승인 대기'), findsOneWidget);
    expect(find.text('에어컨 표시등 켜기'), findsOneWidget);
    expect(find.text('현관문 열기'), findsOneWidget);
    expect(find.text('현관문 열기는 집 안 7인치 화면에서 승인해요'), findsOneWidget);
    expect(find.text('AI 제안'), findsOneWidget);

    final approveButtons = find.widgetWithText(FilledButton, '승인');
    expect(approveButtons, findsNWidgets(2));
    expect(tester.widget<FilledButton>(approveButtons.at(0)).onPressed, isNotNull);
    expect(tester.widget<FilledButton>(approveButtons.at(1)).onPressed, isNull);

    await tester.tap(find.widgetWithText(OutlinedButton, '거절').at(1));
    await tester.pumpAndSettle();
    expect(find.text('현관문 열기'), findsNothing);
    expect(find.text('데모 모드: 실제로 보내지 않았습니다.'), findsOneWidget);
    expect(find.text('1'), findsOneWidget);
  });
}
