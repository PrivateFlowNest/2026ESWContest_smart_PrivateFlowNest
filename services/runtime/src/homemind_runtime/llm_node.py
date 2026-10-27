"""NODE-RPI5-01 LLM node: pfn/v1/ai/llm/request -> EXAONE -> core task request (origin llm) + llm/result.

EXAONE only proposes. Every task it sends needs UI approval in core (task-policy llm_needs_approval).
The transcript is never logged and never forwarded to core.
"""

from __future__ import annotations

import logging
from typing import Any, Mapping

from homemind_exaone.client import ExaoneClient

from . import pfn
from .action_map import ActionMap, control_parameters, scenario_parameters

log = logging.getLogger(__name__)

_FAILURE_STATUS = {"MODEL_UNAVAILABLE": "model_unavailable", "MODEL_OUTPUT_INVALID": "invalid_output"}


class LlmNode:
    def __init__(self, client: ExaoneClient, action_map: ActionMap, *, node_id: str = "NODE-RPI5-01",
                 simulated: bool = False) -> None:
        self.client = client
        self.action_map = action_map
        self.node_id = node_id
        self.simulated = simulated

    def handle(self, request: Mapping[str, Any]) -> list[tuple[str, dict[str, Any]]]:
        pfn.validate(request, "llm-request.schema.json")
        if request["expires_at_ms"] <= pfn.now_ms():
            result = pfn.llm_result(request=request, status="expired", task_id=None, proposal=None,
                                    origin="none", latency_ms=0.0, model_name=self.client.model,
                                    simulated=self.simulated)
            return [(pfn.TOPICS["llm_result"], result)]
        interpretation = self.client.interpret(request["transcript"]["text"], context=request.get("context"))
        proposal = interpretation.proposal
        flags = set(proposal.get("safety_flags", []))
        status = next((value for key, value in _FAILURE_STATUS.items() if key in flags), None)
        messages: list[tuple[str, dict[str, Any]]] = []
        task_id = None
        if status is None:
            intent = proposal.get("intent")
            if intent == "CLARIFY":
                status = "clarify"
            elif intent == "NO_ACTION":
                status = "no_action"
            else:
                resolution = self.action_map.resolve(proposal)
                if resolution.kind in {"device_control", "scenario"}:
                    if resolution.kind == "device_control":
                        task = pfn.task_request(
                            source_node_id=self.node_id,
                            intent="device_control",
                            target_device=resolution.device_id,
                            parameters=control_parameters(resolution, origin="llm", proposal=proposal),
                            confidence=0.7,
                            trace_id=request["trace_id"],
                        )
                    else:
                        task = pfn.task_request(
                            source_node_id=self.node_id,
                            intent=resolution.intent,
                            target_device="HOME-ENV-01",
                            parameters=scenario_parameters(origin="llm", proposal=proposal),
                            confidence=0.7,
                            trace_id=request["trace_id"],
                        )
                    messages.append((pfn.TOPICS["task_request"], task))
                    task_id = task["task_id"]
                    status = "task_requested"
                elif resolution.kind == "query":
                    status = "no_action"
                else:
                    status = "rejected_guard"
                    log.info("LLM proposal not executable: %s", resolution.reason)
        result = pfn.llm_result(
            request=request,
            status=status,
            task_id=task_id,
            proposal=proposal,
            origin=interpretation.origin if interpretation.origin != "fallback" else "guard_fallback",
            latency_ms=interpretation.latency_ms,
            model_name=interpretation.model,
            simulated=self.simulated,
        )
        messages.append((pfn.TOPICS["llm_result"], result))
        log.info("llm result status=%s ai=%s/%s/%s latency_ms=%.0f", status, proposal.get("intent"),
                 proposal.get("device"), proposal.get("action"), interpretation.latency_ms)
        return messages
