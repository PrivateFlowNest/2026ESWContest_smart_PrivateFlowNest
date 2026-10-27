import numpy as np

from homemind_vision.detection import decode_raw_predictions, postprocess, preprocess_bgr


def test_preprocess_preserves_fixed_nchw_shape():
    image = np.zeros((480, 640, 3), dtype=np.uint8)
    tensor, ratio = preprocess_bgr(image, (416, 416))
    assert tensor.shape == (1, 3, 416, 416)
    assert tensor.dtype == np.float32
    assert ratio == 0.65


def test_decode_and_postprocess_one_high_confidence_prediction():
    raw = np.zeros((1, 3549, 7), dtype=np.float32)
    raw[0, 0, :4] = [5.0, 5.0, 0.0, 0.0]
    raw[0, 0, 4:] = [0.9, 0.1, 0.8]

    decoded = decode_raw_predictions(raw, (416, 416))
    detections = postprocess(
        raw,
        input_size=(416, 416),
        ratio=1.0,
        image_shape=(416, 416),
        score_threshold=0.5,
    )

    assert decoded.shape == raw.shape
    assert len(detections) == 1
    assert detections[0].class_id == 1
    assert abs(detections[0].confidence - 0.72) < 1e-6
