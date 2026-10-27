"""YOLOX-Nano 416 experiment for the single HomeMind FIGURE class."""

import os

import torch.nn as nn

from yolox.exp import Exp as YoloXExp


class Exp(YoloXExp):
    def __init__(self):
        super().__init__()
        self.depth = 0.33
        self.width = 0.25
        self.depthwise = True
        self.num_classes = 1
        self.input_size = (416, 416)
        self.test_size = (416, 416)
        self.random_size = (10, 16)
        self.multiscale_range = 3
        self.data_dir = os.environ.get("HOMEMIND_VISION_COCO_DIR")
        if not self.data_dir:
            raise RuntimeError("HOMEMIND_VISION_COCO_DIR is required")
        self.train_ann = "instances_train2017.json"
        self.val_ann = "instances_val2017.json"
        self.test_ann = "instances_test2017.json"
        self.data_num_workers = 8
        self.max_epoch = 150
        self.warmup_epochs = 5
        self.no_aug_epochs = 15
        self.eval_interval = 5
        self.print_interval = 1
        self.mosaic_prob = 0.5
        self.mosaic_scale = (0.5, 1.5)
        self.degrees = 5.0
        self.translate = 0.1
        self.enable_mixup = False
        self.mixup_prob = 0.0
        self.hsv_prob = 0.8
        self.flip_prob = 0.5
        self.test_conf = 0.05
        self.nmsthre = 0.45
        self.seed = 20260903
        self.output_dir = os.environ.get(
            "HOMEMIND_VISION_TRAINING_OUTPUT", "artifacts/training"
        )
        self.exp_name = "homemind-yolox-nano-figure-v1"

    def get_model(self, sublinear=False):
        def init_yolo(module):
            for layer in module.modules():
                if isinstance(layer, nn.BatchNorm2d):
                    layer.eps = 1e-3
                    layer.momentum = 0.03

        if getattr(self, "model", None) is None:
            from yolox.models import YOLOPAFPN, YOLOX, YOLOXHead

            in_channels = [256, 512, 1024]
            backbone = YOLOPAFPN(
                self.depth,
                self.width,
                in_channels=in_channels,
                act=self.act,
                depthwise=True,
            )
            head = YOLOXHead(
                self.num_classes,
                self.width,
                in_channels=in_channels,
                act=self.act,
                depthwise=True,
            )
            self.model = YOLOX(backbone, head)
        self.model.apply(init_yolo)
        self.model.head.initialize_biases(1e-2)
        return self.model
