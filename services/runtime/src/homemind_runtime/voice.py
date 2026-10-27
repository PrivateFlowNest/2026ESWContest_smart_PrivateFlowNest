"""NODE-RPI5-02 deterministic voice parser: final transcript -> core task request or LLM request.

Same rules as the EXAONE guard (services/exaone-edge guardrails + validation), so an utterance the
guard can decide never needs the LLM. Only "None" (no deterministic answer) goes to NODE-RPI5-01.
"""

from __future__ import annotations

import logging
import uuid
from dataclasses import dataclass, field
from typing import Any, Mapping

from homemind_exaone.guardrails import preflight_proposal
from homemind_exaone.validation import (
    ProposalValidationError,
    default_capabilities_path,
    load_json_object,
    validate_proposal,
)

from . import pfn
from .action_map import ActionMap, control_parameters, scenario_parameters

log = logging.getLogger(__name__)

# Scenario intents (contracts/pfn.v1/scenario-rules.json) are not in the EXAONE vocabulary, so the parser
# maps them by keyword. Checked only after the guard's negation/injection/risk checks; core still asks
# the UI before prepare_home/prepare_sleep/night_bed_exit run (task-policy.json).
SCENARIO_KEYWORDS = (
    ("quiet_mode_off", ("조용 모드 해제", "조용 모드 꺼", "회의 끝났", "회의 끝", "회의 다 했")),
    ("prepare_home", ("귀가", "다녀왔", "집에 왔", "나 왔어", "왔어 집")),
    ("prepare_sleep", ("취침", "잘게", "자러 갈", "잘 준비", "자야겠")),
    ("night_bed_exit", ("밤에 화장실", "야간 조명", "화장실 좀 다녀올게", "화장실 가야겠")),
)
_GUARD_STOP_FLAGS = {"NEGATED_REQUEST", "CONFLICTING_REQUEST", "HIGH_RISK"}


@dataclass
class VoiceDecision:
    kind: str  # task_request | llm_request | clarify | no_action | query | unsupported | invalid
    proposal: dict[str, Any] | None
    messages: list[tuple[str, dict[str, Any]]] = field(default_factory=list)
    note: str = ""


class VoiceParser:
    def __init__(self, action_map: ActionMap, *, node_id: str = "NODE-RPI5-02", llm_ttl_ms: int = 10000,
                 simulated: bool = False) -> None:
        self.action_map = action_map
        self.node_id = node_id
        self.llm_ttl_ms = llm_ttl_ms
        self.simulated = simulated
        self.capabilities = load_json_object(default_capabilities_path())

    def handle(
        self,
        text: str,
        *,
        context: Mapping[str, Any] | None = None,
        stt_model_version: str = "keyboard",
        stt_confidence: float | None = None,
        utterance_id: str | None = None,
        trace_id: str | None = None,
    ) -> VoiceDecision:
        text = text.strip()
        if not text:
            return VoiceDecision("no_action", None, note="empty transcript")
        trace_id = trace_id or pfn.new_id()
        proposal = preflight_proposal(text, self.capabilities, context)
        stopped = proposal is not None and _GUARD_STOP_FLAGS & set(proposal.get("safety_flags", []))
        scenario = None if stopped else self._scenario(text)
        if scenario:
            request = pfn.task_request(
                source_node_id=self.node_id,
                intent=scenario,
                target_device="HOME-ENV-01",
                parameters={"origin": "voice_parser", "ai_device": None, "ai_action": None},
                confidence=stt_confidence if stt_confidence is not None else 0.9,
                trace_id=trace_id,
            )
            return VoiceDecision("task_request", None, [(pfn.TOPICS["task_request"], request)], scenario)
        if proposal is None:
            request = pfn.llm_request(
                text=text,
                utterance_id=utterance_id or str(uuid.uuid4()),
                stt_model_version=stt_model_version,
                stt_confidence=stt_confidence,
                parser_result="NO_MATCH",
                context=context,
                ttl_ms=self.llm_ttl_ms,
                trace_id=trace_id,
                simulated=self.simulated,
            )
            return VoiceDecision("llm_request", None, [(pfn.TOPICS["llm_request"], request)], "parser has no rule")
        try:
            proposal = validate_proposal(proposal, capabilities=self.capabilities)
        except ProposalValidationError as error:
            return VoiceDecision("invalid", proposal, note="; ".join(error.errors))
        intent = proposal["intent"]
        if intent == "CLARIFY":
            return VoiceDecision("clarify", proposal, note=proposal.get("clarification_question") or "")
        if intent == "NO_ACTION":
            return VoiceDecision("no_action", proposal, note=",".join(proposal.get("safety_flags", [])))
        resolution = self.action_map.resolve(proposal)
        confidence = stt_confidence if stt_confidence is not None else 0.9
        if resolution.kind == "device_control":
            request = pfn.task_request(
                source_node_id=self.node_id,
                intent="device_control",
                target_device=resolution.device_id,
                parameters=control_parameters(resolution, origin="voice_parser", proposal=proposal),
                confidence=confidence,
                trace_id=trace_id,
                safety_level="caution" if self.action_map.is_door_open(resolution) else "normal",
            )
            return VoiceDecision("task_request", proposal, [(pfn.TOPICS["task_request"], request)])
        if resolution.kind == "scenario":
            request = pfn.task_request(
                source_node_id=self.node_id,
                intent=resolution.intent,
                target_device="HOME-ENV-01",
                parameters=scenario_parameters(origin="voice_parser", proposal=proposal),
                confidence=confidence,
                trace_id=trace_id,
            )
            return VoiceDecision("task_request", proposal, [(pfn.TOPICS["task_request"], request)])
        if resolution.kind == "query":
            return VoiceDecision("query", proposal, note=resolution.device_id or "")
        return VoiceDecision("unsupported", proposal, note=resolution.reason or "")

    @staticmethod
    def _scenario(text: str) -> str | None:
        folded = text.casefold()
        for intent, keywords in SCENARIO_KEYWORDS:
            if any(keyword in folded for keyword in keywords):
                return intent
        return None
