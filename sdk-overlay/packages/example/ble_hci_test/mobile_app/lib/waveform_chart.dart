import 'package:flutter/material.dart';

/// Rolling waveform (-1..1) for live BLE stream demo.
class WaveformChart extends StatelessWidget {
  const WaveformChart({
    super.key,
    required this.samples,
    this.height = 200,
    this.caption,
    this.isLive = false,
  });

  final List<double> samples;
  final double height;
  final String? caption;
  final bool isLive;

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Stack(
          children: [
            Container(
              height: height,
              width: double.infinity,
              decoration: BoxDecoration(
                gradient: const LinearGradient(
                  begin: Alignment.topCenter,
                  end: Alignment.bottomCenter,
                  colors: [Color(0xFF0A1628), Color(0xFF0D1B2A)],
                ),
                borderRadius: BorderRadius.circular(16),
                border: Border.all(
                  color: isLive
                      ? const Color(0xFF26C6DA).withValues(alpha: 0.6)
                      : const Color(0xFF415A77),
                  width: isLive ? 1.5 : 1,
                ),
                boxShadow: isLive
                    ? [
                        BoxShadow(
                          color: const Color(0xFF26C6DA).withValues(alpha: 0.15),
                          blurRadius: 12,
                          spreadRadius: 1,
                        ),
                      ]
                    : null,
              ),
              child: ClipRRect(
                borderRadius: BorderRadius.circular(16),
                child: CustomPaint(
                  painter: _WaveformPainter(samples),
                  child: samples.isEmpty
                      ? Center(
                          child: Column(
                            mainAxisSize: MainAxisSize.min,
                            children: [
                              Icon(
                                Icons.graphic_eq,
                                size: 36,
                                color: Colors.white.withValues(alpha: 0.25),
                              ),
                              const SizedBox(height: 8),
                              Text(
                                '等待实时数据…',
                                style: TextStyle(
                                  color: Colors.white.withValues(alpha: 0.45),
                                  fontSize: 13,
                                ),
                              ),
                            ],
                          ),
                        )
                      : null,
                ),
              ),
            ),
            if (isLive)
              Positioned(
                top: 10,
                right: 10,
                child: Container(
                  padding:
                      const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
                  decoration: BoxDecoration(
                    color: const Color(0xFFE53935).withValues(alpha: 0.9),
                    borderRadius: BorderRadius.circular(20),
                  ),
                  child: const Row(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      Icon(Icons.fiber_manual_record, color: Colors.white, size: 10),
                      SizedBox(width: 6),
                      Text(
                        'LIVE',
                        style: TextStyle(
                          color: Colors.white,
                          fontSize: 11,
                          fontWeight: FontWeight.w700,
                          letterSpacing: 0.8,
                        ),
                      ),
                    ],
                  ),
                ),
              ),
          ],
        ),
        if (caption != null) ...[
          const SizedBox(height: 8),
          Text(
            caption!,
            style: Theme.of(context).textTheme.bodySmall?.copyWith(
                  color: Colors.blueGrey.shade600,
                ),
          ),
        ],
      ],
    );
  }
}

class _WaveformPainter extends CustomPainter {
  _WaveformPainter(this.samples);

  final List<double> samples;

  @override
  void paint(Canvas canvas, Size size) {
    if (samples.length < 2) return;

    final grid = Paint()
      ..color = const Color(0xFF1B263B)
      ..strokeWidth = 1;
    for (var i = 1; i < 4; i++) {
      final y = size.height * i / 4;
      canvas.drawLine(Offset(0, y), Offset(size.width, y), grid);
    }
    canvas.drawLine(
      Offset(0, size.height / 2),
      Offset(size.width, size.height / 2),
      Paint()
        ..color = const Color(0xFF415A77)
        ..strokeWidth = 1,
    );

    final n = samples.length;
    final path = Path();
    final fill = Path();

    for (var i = 0; i < n; i++) {
      final x = size.width * i / (n - 1);
      final v = samples[i].clamp(-1.0, 1.0);
      final y = size.height * (0.5 - v * 0.42);
      if (i == 0) {
        path.moveTo(x, y);
        fill.moveTo(x, size.height / 2);
        fill.lineTo(x, y);
      } else {
        path.lineTo(x, y);
        fill.lineTo(x, y);
      }
    }
    fill.lineTo(size.width, size.height / 2);
    fill.close();

    canvas.drawPath(
      fill,
      Paint()
        ..shader = LinearGradient(
          begin: Alignment.topCenter,
          end: Alignment.bottomCenter,
          colors: [
            const Color(0xFF26C6DA).withValues(alpha: 0.35),
            const Color(0xFF26C6DA).withValues(alpha: 0.02),
          ],
        ).createShader(Rect.fromLTWH(0, 0, size.width, size.height)),
    );

    canvas.drawPath(
      path,
      Paint()
        ..color = const Color(0xFF4DD0E1)
        ..style = PaintingStyle.stroke
        ..strokeWidth = 2.2
        ..strokeJoin = StrokeJoin.round
        ..strokeCap = StrokeCap.round,
    );
  }

  @override
  bool shouldRepaint(covariant _WaveformPainter oldDelegate) =>
      oldDelegate.samples != samples;
}
