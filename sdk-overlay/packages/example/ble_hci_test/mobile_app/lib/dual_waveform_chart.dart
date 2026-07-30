import 'package:flutter/material.dart';

class DualWaveformChart extends StatelessWidget {
  const DualWaveformChart({
    super.key,
    required this.samples735,
    required this.samples850,
    this.title,
    this.height = 120,
    this.isLive = false,
  });

  final List<double> samples735;
  final List<double> samples850;
  final String? title;
  final double height;
  final bool isLive;

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        if (title != null)
          Padding(
            padding: const EdgeInsets.only(bottom: 4),
            child: Text(
              title!,
              style: const TextStyle(fontSize: 12, fontWeight: FontWeight.w600),
            ),
          ),
        SizedBox(
          height: height,
          child: CustomPaint(
            painter: _DualPainter(samples735, samples850, isLive),
            child: samples735.isEmpty && samples850.isEmpty
                ? const Center(
                    child: Text('等待数据…', style: TextStyle(fontSize: 11)),
                  )
                : null,
          ),
        ),
        const SizedBox(height: 2),
        Row(
          children: [
            _legend(const Color(0xFFE53935), '735'),
            const SizedBox(width: 12),
            _legend(const Color(0xFF43A047), '850'),
          ],
        ),
      ],
    );
  }

  Widget _legend(Color c, String t) => Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Container(width: 12, height: 3, color: c),
          const SizedBox(width: 4),
          Text(t, style: const TextStyle(fontSize: 10)),
        ],
      );
}

class _DualPainter extends CustomPainter {
  _DualPainter(this.s735, this.s850, this.isLive);

  final List<double> s735;
  final List<double> s850;
  final bool isLive;

  @override
  void paint(Canvas canvas, Size size) {
    final bg = Paint()..color = const Color(0xFF0D1B2A);
    canvas.drawRRect(
      RRect.fromRectAndRadius(Offset.zero & size, const Radius.circular(12)),
      bg,
    );

    _drawLine(canvas, size, s735, const Color(0xFFE53935));
    _drawLine(canvas, size, s850, const Color(0xFF43A047));

    if (isLive) {
      final border = Paint()
        ..color = const Color(0xFF26C6DA).withValues(alpha: 0.5)
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1.5;
      canvas.drawRRect(
        RRect.fromRectAndRadius(Offset.zero & size, const Radius.circular(12)),
        border,
      );
    }
  }

  void _drawLine(Canvas canvas, Size size, List<double> samples, Color color) {
    if (samples.length < 2) return;

    var minV = samples.reduce((a, b) => a < b ? a : b);
    var maxV = samples.reduce((a, b) => a > b ? a : b);
    var span = maxV - minV;

    // Hub-style zoom: when AC is tiny vs DC (~101000 µV), expand Y window.
    if (span < 80) {
      final mid = (maxV + minV) / 2;
      const half = 40.0;
      minV = mid - half;
      maxV = mid + half;
      span = maxV - minV;
    }
    if (span < 1) span = 1;

    final path = Path();
    for (var i = 0; i < samples.length; i++) {
      final x = i / (samples.length - 1) * size.width;
      final y = size.height - ((samples[i] - minV) / span) * (size.height - 8) - 4;
      if (i == 0) {
        path.moveTo(x, y);
      } else {
        path.lineTo(x, y);
      }
    }

    final paint = Paint()
      ..color = color
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.2;
    canvas.drawPath(path, paint);
  }

  @override
  bool shouldRepaint(covariant _DualPainter old) => true;
}
