import 'dart:math' as math;

class SignalProcOptions {
  const SignalProcOptions({
    this.subtractBias = false,
    this.biasUv = 100000,
    this.detrend = false,
    this.filter = false,
    this.filterLowHz = 0.01,
    this.filterHighHz = 2.0,
    this.sampleRateHz = 8.0,
    this.streamUvPerCount = 10.0,
  });

  final bool subtractBias;
  final int biasUv;
  final bool detrend;
  final bool filter;
  final double filterLowHz;
  final double filterHighHz;
  final double sampleRateHz;
  /// BLE int16 count -> µV (board sends pd24/10).
  final double streamUvPerCount;
}

class SignalProcessor {
  SignalProcessor(this.opts);

  final SignalProcOptions opts;
  double? _hp;
  double? _lp;
  final List<double> _trendBuf = [];
  static const _trendWin = 32;

  double processRaw(int rawScaled) {
    var v = rawScaled * opts.streamUvPerCount;
    if (opts.subtractBias) {
      v -= opts.biasUv;
    }
    return v;
  }

  double processDisplay(double v) {
    if (opts.detrend) {
      _trendBuf.add(v);
      if (_trendBuf.length > _trendWin) {
        _trendBuf.removeAt(0);
      }
      final mean = _trendBuf.reduce((a, b) => a + b) / _trendBuf.length;
      v -= mean;
    }

    if (opts.filter && opts.sampleRateHz > 0) {
      final dt = 1.0 / opts.sampleRateHz;
      final rcHp = 1.0 / (2 * math.pi * opts.filterHighHz);
      final alphaHp = dt / (rcHp + dt);
      _hp = _hp == null ? v : _hp! + alphaHp * (v - _hp!);
      v -= _hp!;

      final rcLp = 1.0 / (2 * math.pi * opts.filterLowHz);
      final alphaLp = dt / (rcLp + dt);
      _lp = _lp == null ? v : _lp! + alphaLp * (v - _lp!);
      v = _lp!;
    }

    return v;
  }

  void reset() {
    _hp = null;
    _lp = null;
    _trendBuf.clear();
  }
}
