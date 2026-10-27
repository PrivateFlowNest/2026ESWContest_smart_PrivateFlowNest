"""Small reproducible acceptance suite for structured EXAONE output."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Callable, Iterable

from .client import ExaoneClient
from .validation import ProposalValidationError, validate_proposal


def load_cases(path: str | Path) -> list[dict[str, Any]]:
    cases: list[dict[str, Any]] = []
    for line_number, line in enumerate(
        Path(path).read_text(encoding="utf-8").splitlines(), start=1
    ):
        if not line.strip():
            continue
        value = json.loads(line)
        if not isinstance(value, dict) or not isinstance(value.get("utterance"), str):
            raise ValueError(f"Invalid evaluation case at line {line_number}")
        if not isinstance(value.get("expected", {}), dict):
            raise ValueError(f"Invalid expected object at line {line_number}")
        cases.append(value)
    if not cases:
        raise ValueError("Evaluation file contains no cases")
    return cases


def _matches(proposal: dict[str, Any], expected: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    for name, wanted in expected.items():
        if name == "safety_flags_contains":
            actual = proposal.get("safety_flags", [])
            missing = [item for item in wanted if item not in actual]
            if missing:
                errors.append(f"safety_flags missing {missing}")
            continue
        if name == "clarification_question" and isinstance(wanted, str):
            actual = proposal.get(name)
            if not isinstance(actual, str) or not actual.strip():
                errors.append("clarification_question: expected a non-empty question")
            continue
        actual = proposal.get(name)
        # `safety_flags` is itself an array-valued contract field. Other list
        # expectations mean "one of these scalar alternatives" (for example,
        # either DEVICE_CONTROL or CONTEXT_SUGGESTION). Treating every list as
        # alternatives made an exact empty safety flag list fail every case.
        if name == "safety_flags":
            if actual != wanted:
                errors.append(f"{name}: expected {wanted!r}, got {actual!r}")
        elif isinstance(wanted, list):
            if actual not in wanted:
                errors.append(f"{name}: expected one of {wanted!r}, got {actual!r}")
        elif actual != wanted:
            errors.append(f"{name}: expected {wanted!r}, got {actual!r}")
    return errors


def _percentile(values: Iterable[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0.0
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def evaluate_cases(
    client: ExaoneClient,
    cases: list[dict[str, Any]],
    *,
    progress: Callable[[str], None] | None = None,
) -> dict[str, Any]:
    results: list[dict[str, Any]] = []
    accepted_count = 0
    contract_success_count = 0
    guard_count = 0
    model_call_count = 0
    model_valid_count = 0
    model_first_pass_count = 0
    retry_recovered_count = 0
    matched_count = 0
    latencies: list[float] = []
    category_counts: dict[str, dict[str, int]] = {}
    for index, case in enumerate(cases, start=1):
        result = client.interpret(
            case["utterance"], context=case.get("context", {})
        )
        try:
            validate_proposal(
                result.proposal,
                schema=getattr(client, "schema", None),
                capabilities=getattr(client, "capabilities", None),
            )
            final_contract_valid = True
        except ProposalValidationError:
            final_contract_valid = False
        match_errors = _matches(result.proposal, case.get("expected", {}))
        accepted_count += int(result.accepted)
        contract_success_count += int(final_contract_valid)
        guard_count += int(result.origin in {"guard", "model_guard", "guard_fallback"})
        model_call_count += int(result.attempts > 0)
        model_valid_count += int(result.model_output_valid is True)
        model_first_pass_count += int(
            result.model_output_valid is True
            and result.attempts == 1
        )
        retry_recovered_count += int(
            result.model_output_valid is True
            and result.attempts > 1
        )
        matched_count += int(result.accepted and not match_errors)
        category = str(case.get("category", "uncategorized"))
        family = category.split("/", 1)[0]
        bucket = category_counts.setdefault(
            family, {"total": 0, "matched": 0, "contract_valid": 0}
        )
        bucket["total"] += 1
        bucket["matched"] += int(result.accepted and not match_errors)
        bucket["contract_valid"] += int(final_contract_valid)
        latencies.append(result.latency_ms)
        results.append(
            {
                "id": case.get("id", f"case-{index:03d}"),
                "category": category,
                "accepted": result.accepted,
                "final_contract_valid": final_contract_valid,
                "origin": result.origin,
                "model_output_valid": result.model_output_valid,
                "expectation_match": result.accepted and not match_errors,
                "attempts": result.attempts,
                "latency_ms": round(result.latency_ms, 3),
                "proposal": result.proposal,
                "validation_errors": list(result.errors),
                "expectation_errors": match_errors,
            }
        )
        if progress:
            progress(
                f"[{index}/{len(cases)}] {results[-1]['id']}: "
                f"accepted={result.accepted} origin={result.origin} "
                f"match={not match_errors} "
                f"latency={result.latency_ms:.0f}ms"
            )

    total = len(cases)
    category_metrics = {
        family: {
            "total": values["total"],
            "expectation_success_rate": values["matched"] / values["total"],
            "contract_success_rate": values["contract_valid"] / values["total"],
        }
        for family, values in sorted(category_counts.items())
    }
    return {
        "model": client.model,
        "base_url": client.base_url,
        "total": total,
        "contract_success_count": contract_success_count,
        "contract_success_rate": contract_success_count / total,
        "model_acceptance_count": accepted_count,
        "model_acceptance_rate": accepted_count / total,
        "guard_count": guard_count,
        "model_call_count": model_call_count,
        "model_first_pass_success_count": model_first_pass_count,
        "model_first_pass_success_rate": (
            model_first_pass_count / model_call_count if model_call_count else None
        ),
        "retry_recovered_count": retry_recovered_count,
        "model_schema_success_count": model_valid_count,
        "model_schema_success_rate": (
            model_valid_count / model_call_count if model_call_count else None
        ),
        "expectation_success_count": matched_count,
        "expectation_success_rate": matched_count / total,
        "latency_ms": {
            "p50": round(_percentile(latencies, 0.50), 3),
            "p95": round(_percentile(latencies, 0.95), 3),
            "max": round(max(latencies, default=0.0), 3),
        },
        "category_metrics": category_metrics,
        "results": results,
    }


def check_targets(
    report: dict[str, Any], targets: dict[str, Any]
) -> list[str]:
    """Return human-readable gate failures for a completed evaluation report."""

    failures: list[str] = []
    overall = targets.get("overall", {})
    for metric, minimum in overall.items():
        actual = report.get(metric)
        if actual is None or float(actual) < float(minimum):
            failures.append(f"{metric}: {actual!r} < {minimum!r}")

    categories = targets.get("categories", {})
    report_categories = report.get("category_metrics", {})
    for family, requirements in categories.items():
        actual_metrics = report_categories.get(family)
        if not isinstance(actual_metrics, dict):
            failures.append(f"category {family}: missing")
            continue
        for metric, minimum in requirements.items():
            actual = actual_metrics.get(metric)
            if actual is None or float(actual) < float(minimum):
                failures.append(
                    f"category {family}.{metric}: {actual!r} < {minimum!r}"
                )
    return failures
