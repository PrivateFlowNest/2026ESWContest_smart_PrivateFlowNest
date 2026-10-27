from pathlib import Path

import pytest

SERVICE_ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="session")
def trained_baseline() -> tuple[Path, Path]:
    data = SERVICE_ROOT / "data" / "partial-intent-v2"
    model = SERVICE_ROOT / "artifacts" / "partial-intent-v2.onnx"
    assert data.is_dir(), "run the dataset command before pytest"
    assert model.is_file(), "run the train command before pytest"
    return data, model
