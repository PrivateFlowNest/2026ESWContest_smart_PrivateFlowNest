import 'models.dart';
import 'phone_pairing.dart';

/// The UI only knows this interface.
///
/// Laptop testing uses [MockHomeMindDataSource]. On Pi5, replace it with a
/// D-Bus implementation that talks to the local C++ bridge. No screen code
/// needs to change.
abstract class HomeMindDataSource {
  bool get isLive;

  Stream<HomeSnapshot> get snapshots;

  HomeSnapshot get current;

  Future<void> start();

  Future<void> requestSnapshot();

  Future<void> actOnTask(String taskId, TaskAction action);

  Future<void> simulateArrival();

  Future<void> proposeSleep();

  Future<void> simulateNightExit();

  Future<void> simulateCommandTimeout();

  Future<void> simulateDeviceOffline();

  Future<void> triggerSafetyAlert();

  Future<void> acknowledgeSafetyAlert();

  Future<void> setPrivacyMode(PrivacyMode mode);

  /// Push-to-talk: asks NODE-RPI5-02 to listen for one utterance (or stop).
  /// Returns false when nothing was sent (offline).
  Future<bool> requestVoiceListen(VoiceListenAction action);

  /// Phone pairing QR: asks the Mobile API on NODE-RPI4-01 for a one-time
  /// offer. Throws [PhonePairingException] (Korean message) when there is none.
  Future<PhonePairingOffer> requestPhonePairing();

  /// Phones paired with the Mobile API (names, pairing time, last request). Throws [PhonePairingException]
  /// (Korean message) when there is no answer.
  Future<PairedPhones> requestPairedPhones();

  /// Unpairs [phoneId]: its token stops working at once and the phone has to scan a new QR. Returns the list after.
  Future<PairedPhones> unpairPhone(String phoneId);

  /// The list the Mobile API sends by itself right after a phone paired.
  Stream<PairedPhones> get pairedPhoneUpdates;

  Future<void> submitFeedback({
    required FeedbackType type,
    required FeedbackRating rating,
    String? taskId,
    String note,
  });

  Future<void> setConnected(bool connected);

  Future<void> dispose();
}

/// Laptop-only rehearsal events for the core 0.6.0 task kinds. Only the mock
/// data source implements this; the live MQTT source never fakes AI, voice
/// or perception messages.
abstract class HomeMindDemoActions {
  /// Voice parser `device_control` that runs without approval.
  Future<void> demoVoiceCommand();

  /// EXAONE (`origin: llm`) proposal that waits for 실행/취소.
  Future<void> demoAiProposal();

  /// `ai/llm/result` with status clarify.
  Future<void> demoAiClarify();

  /// Voice request to open SERVO-DOOR-01, which always needs approval.
  Future<void> demoEntranceDoorRequest();

  /// Request for a not_wired device, rejected with DEVICE_NOT_WIRED.
  Future<void> demoUnwiredRequest();

  /// Phone app (`origin: ui` from NODE-RPI4-01) command.
  Future<void> demoPhoneCommand();

  /// ACTIVITY_CONTEXT meeting → core context rule quiet_mode_on.
  Future<void> demoMeetingStarted();

  /// ACTIVITY_CONTEXT deactivated → quiet_mode_off.
  Future<void> demoMeetingEnded();

  /// PARTIAL_INTENT_HINT while speaking ("환기 준비 중"), then the final voice command.
  Future<void> demoPartialHint();
}
