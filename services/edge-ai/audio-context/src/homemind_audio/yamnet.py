"""Lazy TensorFlow Hub wrapper for YAMNet embeddings."""

from __future__ import annotations

from pathlib import Path

import numpy as np

from .audio import (
    load_wav_16k_mono,
    load_wav_segment_16k_mono,
    vad_temporal_features,
)

YAMNET_HANDLE = "https://tfhub.dev/google/yamnet/1"
FEATURE_VERSION_MEAN_VAD_V1 = "mean-vad-v1"
FEATURE_VERSION_TEMPORAL_V2 = "mean-std-max-vad-v2"
FEATURE_VERSIONS = (
    FEATURE_VERSION_MEAN_VAD_V1,
    FEATURE_VERSION_TEMPORAL_V2,
)


class YamnetEmbeddingExtractor:
    """Extract a fixed-size YAMNet and VAD feature vector from a WAV clip."""

    def __init__(
        self,
        model_handle: str = YAMNET_HANDLE,
        *,
        feature_version: str = FEATURE_VERSION_MEAN_VAD_V1,
    ) -> None:
        if feature_version not in FEATURE_VERSIONS:
            raise ValueError(
                f"Unknown YAMNet feature version {feature_version!r}; "
                f"expected one of {FEATURE_VERSIONS}"
            )
        try:
            import tensorflow as tf
            import tensorflow_hub as hub
        except ImportError as exc:
            raise RuntimeError(
                f"TensorFlow Hub import failed: {exc}. "
                "Install the project dependencies in the project virtual environment."
            ) from exc

        self.model_handle = model_handle
        self.feature_version = feature_version
        # Preserve the original cache identity so v1-v3 models keep reusing their
        # existing mean+VAD embeddings. Temporal v4 features get an isolated cache.
        self.cache_identity = (
            model_handle
            if feature_version == FEATURE_VERSION_MEAN_VAD_V1
            else f"{model_handle}::{feature_version}"
        )
        self._tf = tf
        for device in tf.config.list_physical_devices("GPU"):
            try:
                tf.config.experimental.set_memory_growth(device, True)
            except RuntimeError:
                # TensorFlow may already have initialized the device in an embedding
                # worker. In that case its existing allocation policy is retained.
                pass
        self._model = hub.load(model_handle)

        @tf.function(reduce_retracing=True)
        def infer_padded_batch(waveforms, lengths):
            def infer_one(inputs):
                waveform, length = inputs
                _, embeddings, _ = self._model(waveform[:length])
                mean = tf.reduce_mean(embeddings, axis=0)
                if self.feature_version == FEATURE_VERSION_TEMPORAL_V2:
                    return tf.concat(
                        [
                            mean,
                            tf.math.reduce_std(embeddings, axis=0),
                            tf.reduce_max(embeddings, axis=0),
                        ],
                        axis=0,
                    )
                return mean

            return tf.map_fn(
                infer_one,
                (waveforms, lengths),
                fn_output_signature=tf.TensorSpec(
                    (self.embedding_feature_dimensions,), tf.float32
                ),
                parallel_iterations=32,
            )

        self._infer_padded_batch = infer_padded_batch

    @property
    def embedding_feature_dimensions(self) -> int:
        return 3072 if self.feature_version == FEATURE_VERSION_TEMPORAL_V2 else 1024

    @property
    def feature_dimensions(self) -> int:
        return self.embedding_feature_dimensions + 12

    def _pool_embeddings(self, embeddings):
        mean = self._tf.reduce_mean(embeddings, axis=0)
        if self.feature_version == FEATURE_VERSION_TEMPORAL_V2:
            return self._tf.concat(
                [
                    mean,
                    self._tf.math.reduce_std(embeddings, axis=0),
                    self._tf.reduce_max(embeddings, axis=0),
                ],
                axis=0,
            )
        return mean

    def extract_waveform(self, waveform: np.ndarray) -> np.ndarray:
        _, embeddings, _ = self._model(waveform)
        values = np.asarray(embeddings.numpy(), dtype=np.float32)
        if values.ndim != 2 or values.shape[0] == 0:
            raise RuntimeError(f"Unexpected YAMNet embedding shape: {values.shape}")
        pooled = np.asarray(self._pool_embeddings(embeddings).numpy(), dtype=np.float32)
        return np.concatenate(
            [pooled, vad_temporal_features(waveform)]
        ).astype(np.float32, copy=False)

    def extract_waveforms(self, waveforms: list[np.ndarray]) -> np.ndarray:
        """Extract an equivalent feature matrix for a padded waveform batch."""

        if not waveforms:
            return np.empty((0, self.feature_dimensions), dtype=np.float32)
        if len(waveforms) == 1:
            return self.extract_waveform(waveforms[0]).reshape(1, -1)

        normalized = [
            np.asarray(waveform, dtype=np.float32).reshape(-1)
            for waveform in waveforms
        ]
        lengths = np.asarray([waveform.size for waveform in normalized], dtype=np.int32)
        padded = np.zeros((len(normalized), int(lengths.max())), dtype=np.float32)
        for index, waveform in enumerate(normalized):
            padded[index, : waveform.size] = waveform

        pooled = np.asarray(
            self._infer_padded_batch(
                self._tf.convert_to_tensor(padded),
                self._tf.convert_to_tensor(lengths),
            ).numpy(),
            dtype=np.float32,
        )
        temporal = np.stack([vad_temporal_features(item) for item in normalized])
        return np.concatenate([pooled, temporal], axis=1).astype(
            np.float32, copy=False
        )

    def extract_file(self, path: str | Path) -> np.ndarray:
        return self.extract_waveform(load_wav_16k_mono(path))

    def extract_segment(
        self, path: str | Path, start_seconds: float, end_seconds: float
    ) -> np.ndarray:
        return self.extract_waveform(
            load_wav_segment_16k_mono(path, start_seconds, end_seconds)
        )
