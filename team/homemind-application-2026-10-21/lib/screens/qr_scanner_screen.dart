import 'package:flutter/material.dart';
import 'package:mobile_scanner/mobile_scanner.dart';

import '../core/theme/homemind_theme.dart';

class QrScannerScreen extends StatefulWidget {
  const QrScannerScreen({super.key});

  @override
  State<QrScannerScreen> createState() => _QrScannerScreenState();
}

class _QrScannerScreenState extends State<QrScannerScreen> {
  bool _handled = false;

  void _onDetect(BarcodeCapture capture) {
    if (_handled) return;
    for (final barcode in capture.barcodes) {
      final value = barcode.rawValue;
      if (value == null || value.trim().isEmpty) continue;
      _handled = true;
      Navigator.of(context).pop(value);
      return;
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.black,
      appBar: AppBar(
        title: const Text('HomeMind QR 스캔'),
        backgroundColor: Colors.black,
      ),
      body: Stack(
        fit: StackFit.expand,
        children: [
          MobileScanner(onDetect: _onDetect),
          Container(color: Colors.black.withValues(alpha: 0.24)),
          Center(
            child: Container(
              width: 270,
              height: 270,
              decoration: BoxDecoration(
                border: Border.all(color: HomeMindColors.mint, width: 3),
                borderRadius: BorderRadius.circular(24),
                color: Colors.transparent,
              ),
            ),
          ),
          Positioned(
            left: 24,
            right: 24,
            bottom: 54,
            child: Container(
              padding: const EdgeInsets.all(16),
              decoration: BoxDecoration(
                color: HomeMindColors.background.withValues(alpha: 0.88),
                borderRadius: BorderRadius.circular(16),
              ),
              child: const Text(
                'Pi 4 화면에 표시된 HomeMind 페어링 QR을 사각형 안에 맞춰주세요.',
                textAlign: TextAlign.center,
                style: TextStyle(color: Colors.white, height: 1.45),
              ),
            ),
          ),
        ],
      ),
    );
  }
}
