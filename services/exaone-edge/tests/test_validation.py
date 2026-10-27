import pytest

from homemind_exaone.validation import (
    ProposalValidationError,
    safe_fallback,
    validate_activity_context,
    validate_proposal,
)


def valid_light_proposal():
    return {
        "intent": "DEVICE_CONTROL",
        "device": "LIGHT",
        "action": "TURN_ON",
        "parameters": {},
        "ambiguous": False,
        "safety_flags": [],
        "clarification_question": None,
    }


def test_valid_device_proposal_is_accepted():
    assert validate_proposal(valid_light_proposal())["action"] == "TURN_ON"


def test_unknown_field_is_rejected():
    proposal = valid_light_proposal()
    proposal["executed"] = True
    with pytest.raises(ProposalValidationError, match="unknown field: executed"):
        validate_proposal(proposal)


def test_disabled_device_is_rejected_even_if_schema_knows_it():
    proposal = valid_light_proposal()
    proposal.update(device="ROBOT_VACUUM", action="START")
    with pytest.raises(ProposalValidationError, match="device is unavailable"):
        validate_proposal(proposal)


def test_device_action_pair_is_enforced():
    proposal = valid_light_proposal()
    proposal["action"] = "OPEN"
    with pytest.raises(ProposalValidationError, match="not allowed for LIGHT"):
        validate_proposal(proposal)


def test_clarify_requires_null_target_and_question():
    proposal = valid_light_proposal()
    proposal.update(
        intent="CLARIFY",
        ambiguous=True,
        clarification_question="어느 기기를 말씀하셨나요?",
    )
    with pytest.raises(ProposalValidationError, match="must not contain device"):
        validate_proposal(proposal)


def test_numeric_parameters_are_bounded_and_required():
    proposal = valid_light_proposal()
    proposal.update(action="SET_BRIGHTNESS", parameters={"brightness": 101})
    with pytest.raises(ProposalValidationError, match="above maximum"):
        validate_proposal(proposal)
    proposal["parameters"] = {}
    with pytest.raises(ProposalValidationError, match="requires parameters.brightness"):
        validate_proposal(proposal)


def test_negated_request_cannot_become_control():
    proposal = valid_light_proposal()
    proposal["safety_flags"] = ["NEGATED_REQUEST"]
    with pytest.raises(ProposalValidationError, match="must use intent=NO_ACTION"):
        validate_proposal(proposal)


def test_unsupported_action_flag_cannot_mark_valid_control_as_executable():
    proposal = valid_light_proposal()
    proposal["safety_flags"] = ["UNSUPPORTED_ACTION"]
    with pytest.raises(ProposalValidationError, match="must use intent=CLARIFY"):
        validate_proposal(proposal)


def test_safe_fallback_itself_passes_validation():
    fallback = safe_fallback(unavailable=True)
    assert validate_proposal(fallback) == fallback


def test_empty_activity_context_is_normalized():
    context = validate_activity_context({})
    assert context["presence_zone"] == "UNKNOWN"
    assert context["quiet_required"] is False


def test_raw_media_cannot_enter_activity_context():
    with pytest.raises(ProposalValidationError, match="unknown field: image"):
        validate_activity_context({"image": "raw-frame"})


def test_activity_context_bounds_confidence():
    with pytest.raises(ProposalValidationError, match="above maximum"):
        validate_activity_context({"confidence": 1.5})


def test_quiet_mode_can_be_resumed_after_context_ends():
    proposal = valid_light_proposal()
    proposal.update(
        intent="DEVICE_CONTROL",
        device="ALL_QUIET_DEVICES",
        action="RESUME",
        parameters={"reason": "회의 상황 종료"},
    )
    assert validate_proposal(proposal)["action"] == "RESUME"
