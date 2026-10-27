"""Mutation tests: each broken copy of the contracts must fail validation.

Run: python -m unittest discover -s contracts/tests
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any, Callable

CONTRACTS = Path(__file__).resolve().parents[1]


def run_validator(root: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(root / "validate-contracts.py")],
        capture_output=True,
        text=True,
        encoding="utf-8",
        check=False,
    )


class ValidateContractsTest(unittest.TestCase):
    def mutate(self, relative: str, change: Callable[[Any], None]) -> str:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "contracts"
            shutil.copytree(CONTRACTS, root, ignore=shutil.ignore_patterns("tests", "__pycache__"))
            path = root / relative
            document = json.loads(path.read_text(encoding="utf-8"))
            change(document)
            path.write_text(json.dumps(document, ensure_ascii=False), encoding="utf-8")
            result = run_validator(root)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("CONTRACT_VALIDATION_FAILED", result.stdout)
        return result.stdout

    def test_current_contracts_pass(self) -> None:
        result = run_validator(CONTRACTS)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("CONTRACT_VALIDATION_PASS", result.stdout)

    def test_core_subscribing_to_voice_listen_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["core_subscriptions"].append("pfn/v1/ai/#")
        self.assertIn("would deliver pfn/v1/ai/voice/listen", self.mutate("pfn.v1/protocol.json", change))

    def test_feedback_override_that_does_not_undo_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["feedback_overrides"][0]["intent"] = "quiet_mode_on"
        self.assertIn("intent must undo active_intent", self.mutate("pfn.v1/context-rules.json", change))

    def test_feedback_override_ending_on_the_wrong_rule_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["feedback_overrides"][0]["suppress_until_rule"] = "quiet_required_on"
        self.assertIn("must end the context", self.mutate("pfn.v1/context-rules.json", change))

    def test_core_subscribing_to_pairing_offer_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["core_subscriptions"].append("pfn/v1/mobile/#")
        self.assertIn("would deliver pfn/v1/mobile/pairing/offer", self.mutate("pfn.v1/protocol.json", change))

    def test_pairing_secret_readable_by_another_account_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["topic_policies"]["mobile_pairing_offer"]["acl_accounts"].append("NODE-RPI5-02")
        self.assertIn("carries a secret", self.mutate("pfn.v1/protocol.json", change))

    def test_unavailable_offer_with_a_code_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["status"] = "unavailable"
            doc["reason"] = "public_base_url is not set"
        self.assertIn("mobile-pairing-offer.json", self.mutate("pfn.v1/examples/mobile-pairing-offer.json", change))

    def test_offer_for_another_request_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["request_id"] = "5f0e0000-0000-4000-8000-0000000000c0"
        self.assertIn("request_id does not answer", self.mutate("pfn.v1/examples/mobile-pairing-offer.json", change))

    def test_offer_code_differing_from_qr_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["pairing_code"] = "ABCD-EFGH"
        self.assertIn("qr_payload pairing_code differs", self.mutate("pfn.v1/examples/mobile-pairing-offer.json", change))

    def test_voice_listen_from_wrong_node_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["source_node_id"] = "NODE-RPI5-02"
        self.assertIn("voice-listen-start.json", self.mutate("pfn.v1/examples/voice-listen-start.json", change))

    def test_scenario_on_not_wired_device_fails(self) -> None:
        def change(doc: Any) -> None:
            command = doc["scenarios"][2]["commands"][0]
            command["target_device_id"] = "LED-GUIDE-01"
        self.assertIn("not_wired", self.mutate("pfn.v1/scenario-rules.json", change))

    def test_scenario_unregistered_servo_angle_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["scenarios"][1]["commands"][2]["parameters"]["position_degrees"] = 45
        self.assertIn("not a registered open/closed position", self.mutate("pfn.v1/scenario-rules.json", change))

    def test_fan_level_without_capability_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["scenarios"][0]["commands"][1] = {
                "target_node_id": "CTRL-ESP8266-LED-01",
                "target_device_id": "FAN-01",
                "action": "set_level",
                "parameters": {"level_percent": 30},
            }
        self.assertIn("set_level is not a capability of FAN-01", self.mutate("pfn.v1/scenario-rules.json", change))

    def test_unsupported_led_colour_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["scenarios"][3]["commands"][0]["parameters"]["color"] = "green"
        self.assertIn("not supported by LED-AIRCON-01", self.mutate("pfn.v1/scenario-rules.json", change))

    def test_perception_unknown_zone_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["zone"] = "kitchen"
        self.assertIn("not a registry zone", self.mutate("pfn.v1/examples/perception-activity-context.json", change))

    def test_perception_with_coordinates_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["attributes"]["bbox"] = [1, 2, 3, 4]
        self.assertIn("unexpected property bbox", self.mutate("pfn.v1/examples/perception-figure-presence.json", change))

    def test_llm_request_from_core_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["source_node_id"] = "NODE-RPI4-01"
        self.mutate("pfn.v1/examples/llm-request.json", change)

    def test_llm_request_without_expiry_fails(self) -> None:
        def change(doc: Any) -> None:
            del doc["expires_at_ms"]
        self.assertIn("expires_at_ms", self.mutate("pfn.v1/examples/llm-request.json", change))

    def test_llm_result_must_not_carry_transcript(self) -> None:
        def change(doc: Any) -> None:
            doc["transcript"] = {"text": "x"}
        self.assertIn("unexpected property transcript", self.mutate("pfn.v1/examples/llm-result-task.json", change))

    def test_core_subscribing_llm_topic_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["topic_policies"]["ai_llm_request"]["core_subscribes"] = True
        self.mutate("pfn.v1/protocol.json", change)

    def test_device_control_target_mismatch_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["target_device"] = "LED-TV-01"
        self.assertIn("target_device must equal", self.mutate("pfn.v1/examples/task-request-device-control.json", change))

    def test_device_control_from_wrong_node_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["source_node_id"] = "NODE-RPI5-02"
        self.assertIn("origin llm must come from NODE-RPI5-01", self.mutate("pfn.v1/examples/task-request-device-control.json", change))

    def test_enabling_pending_fan_level_mapping_fails(self) -> None:
        def change(doc: Any) -> None:
            for entry in doc["actions"]:
                if entry["ai_device"] == "VENTILATION_FAN" and entry["ai_action"] == "SET_LEVEL":
                    entry["enabled"] = True
                    entry["parameters"] = {"level_percent": 30}
        self.assertIn("set_level is not a capability of FAN-01", self.mutate("pfn.v1/ai-action-map.json", change))

    def test_action_map_missing_ai_device_fails(self) -> None:
        def change(doc: Any) -> None:
            del doc["devices"]["ROBOT_VACUUM"]
        self.assertIn("ROBOT_VACUUM has no device entry", self.mutate("pfn.v1/ai-action-map.json", change))

    def test_registry_duplicate_pin_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["devices"][2]["pin"] = "GPIO19"  # SERVO-WINDOW-01 onto SERVO-DOOR-01's pin
        self.assertIn("share a controller pin", self.mutate("pfn.v1/device-registry.json", change))

    def test_source_direction_is_rejected(self) -> None:
        def change(doc: Any) -> None:
            doc["properties"]["event_type"]["enum"].append("SOURCE_DIRECTION")
        self.mutate("rpi5-internal/context-feature-observation.schema.json", change)

    def test_transcript_old_scope_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["scope"] = "PI5_LOCAL_ONLY"
        self.mutate("rpi5-internal/examples/speech-transcript.json", change)

    def test_internal_event_without_node_fails(self) -> None:
        def change(doc: Any) -> None:
            del doc["source_node_id"]
        self.mutate("rpi5-internal/examples/audio-context.json", change)

    def test_core_wildcard_subscription_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["core_subscriptions"] = ["pfn/v1/#"]
        self.assertIn("would deliver pfn/v1/ai/llm/request", self.mutate("pfn.v1/protocol.json", change))

    def test_core_missing_subscription_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["core_subscriptions"].remove("pfn/v1/ai/perception")
        self.assertIn("miss pfn/v1/ai/perception", self.mutate("pfn.v1/protocol.json", change))

    def test_policy_auto_llm_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["rules"] = [rule for rule in doc["rules"] if rule["name"] != "llm_needs_approval"]
            doc["rules"].append({"name": "llm_auto", "origins": ["llm"], "approval": "auto"})
        self.assertIn("decided ui_approval", self.mutate("pfn.v1/task-policy.json", change))

    def test_policy_entrance_door_auto_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["rules"].pop(0)
        self.assertIn("SERVO-DOOR-01/open", self.mutate("pfn.v1/task-policy.json", change))

    def test_policy_phone_opening_the_door_fails(self) -> None:
        def change(doc: Any) -> None:
            del doc["rules"][0]["phone_may_approve"]
        self.assertIn("phone approval of device_control/ui/SERVO-DOOR-01/open must be refused",
                      self.mutate("pfn.v1/task-policy.json", change))

    def test_policy_phone_flag_on_auto_rule_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["rules"][2]["phone_may_approve"] = False
        self.assertIn("phone_may_approve only applies", self.mutate("pfn.v1/task-policy.json", change))

    def test_policy_door_waiting_like_other_proposals_fails(self) -> None:
        def change(doc: Any) -> None:
            del doc["rules"][0]["proposal_ttl_seconds"]
        self.assertIn("device_control/ui/SERVO-DOOR-01/open waits 600 s for approval, decided 120 s",
                      self.mutate("pfn.v1/task-policy.json", change))

    def test_policy_without_default_proposal_ttl_fails(self) -> None:
        def change(doc: Any) -> None:
            del doc["proposal_ttl_seconds"]
        self.assertIn("proposal_ttl_seconds", self.mutate("pfn.v1/task-policy.json", change))

    def test_policy_ttl_on_auto_rule_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["rules"][2]["proposal_ttl_seconds"] = 300
        self.assertIn("proposal_ttl_seconds only applies", self.mutate("pfn.v1/task-policy.json", change))

    def test_policy_without_expired_code_fails(self) -> None:
        def change(doc: Any) -> None:
            del doc["rejection_codes"]["PROPOSAL_EXPIRED"]
        self.assertIn("PROPOSAL_EXPIRED is missing", self.mutate("pfn.v1/task-policy.json", change))

    def test_mobile_task_action_with_other_schema_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["message_schemas"]["mobile_task_action"] = "feedback.schema.json"
        self.assertIn("must carry the ui_action document", self.mutate("pfn.v1/protocol.json", change))

    def test_core_subscribing_to_phone_list_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["core_subscriptions"].append("pfn/v1/mobile/#")
        self.assertIn("core must not subscribe to mobile_phones_state", self.mutate("pfn.v1/protocol.json", change))

    def test_unpaired_phone_still_listed_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["phones"].append({"phone_id": "3f9a0c12", "device_name": "old phone",
                                  "paired_at": "2026-10-24T08:00:00Z", "last_seen_at_ms": None})
        self.assertIn("the unpaired phone is still listed", self.mutate("pfn.v1/examples/mobile-phones-state.json", change))

    def test_phone_state_for_another_request_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["request_id"] = "5f0e0000-0000-4000-8000-0000000000ff"
        self.assertIn("does not answer mobile-phones-request.json",
                      self.mutate("pfn.v1/examples/mobile-phones-state.json", change))

    def test_core_missing_mobile_task_action_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["core_subscriptions"].remove("pfn/v1/mobile/task/action")
        self.assertIn("miss pfn/v1/mobile/task/action", self.mutate("pfn.v1/protocol.json", change))

    def test_policy_unknown_node_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["origins"]["ui"]["allowed_source_nodes"].append("NODE-PHONE-99")
        self.assertIn("unknown node NODE-PHONE-99", self.mutate("pfn.v1/task-policy.json", change))

    def test_context_rule_unknown_intent_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["rules"][0]["intent"] = "make_coffee"
        self.assertIn("make_coffee is not a scenario", self.mutate("pfn.v1/context-rules.json", change))

    def test_context_rule_bad_attribute_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["rules"][0]["match"]["transition"] = "STARTED"
        self.assertIn("not a valid transition", self.mutate("pfn.v1/context-rules.json", change))

    def test_rejected_update_unknown_code_fails(self) -> None:
        def change(doc: Any) -> None:
            doc["task"]["failure"]["code"] = "SOMETHING_ELSE"
        self.assertIn("rejection_codes", self.mutate("pfn.v1/examples/task-update-rejected.json", change))


if __name__ == "__main__":
    unittest.main()
