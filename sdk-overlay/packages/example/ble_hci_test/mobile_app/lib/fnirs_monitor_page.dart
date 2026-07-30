import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import 'ble_board_rx.dart';
import 'ble_connect.dart';
import 'ble_live_sync.dart';
import 'ble_stream.dart';
import 'ble_transfer.dart';
import 'dual_waveform_chart.dart';
import 'fnirs_ble.dart';
import 'fnirs_channel_filter.dart';
import 'fnirs_led_array_dialog.dart';
import 'protocol.dart';
import 'signal_proc.dart';

class FnirsMonitorPage extends StatefulWidget {
  const FnirsMonitorPage({super.key, this.device});

  final BluetoothDevice? device;

  @override
  State<FnirsMonitorPage> createState() => _FnirsMonitorPageState();
}

class _ChannelPlot {
  _ChannelPlot(this.ch, SignalProcOptions opts)
      : proc735 = SignalProcessor(opts),
        proc850 = SignalProcessor(opts);

  final FnirsChannel ch;
  final SignalProcessor proc735;
  final SignalProcessor proc850;
  final List<double> w735 = [];
  final List<double> w850 = [];
  static const cap = 200;

  void pushPair(int raw735, int raw850) {
    final v735 = proc735.processDisplay(proc735.processRaw(raw735));
    final v850 = proc850.processDisplay(proc850.processRaw(raw850));
    w735.add(v735);
    w850.add(v850);
    while (w735.length > cap) {
      w735.removeAt(0);
      w850.removeAt(0);
    }
  }

  void reset() {
    proc735.reset();
    proc850.reset();
    w735.clear();
    w850.clear();
  }
}

class _FnirsMonitorPageState extends State<FnirsMonitorPage> {
  final _fnirs = FnirsBleService();
  final _stream = BleStreamService();
  final _rx = BleBoardRxService();
  final _logs = <String>[];
  BleLiveSyncService? _liveSync;

  static const _maxDisplay = 4;
  static const _maxBlePreview = 4;

  List<bool> _alive = List.filled(12, false);

  List<FnirsChannel> _displayChannels =
      List.of(FnirsChannelExt.defaultDisplaySet);
  List<FnirsChannel> _blePreviewChannels =
      List.of(FnirsChannelExt.defaultPreviewSet);

  late List<_ChannelPlot> _plots;

  bool _busy = false;
  bool _acquiring = false;
  bool _downloading = false;
  bool _downloadPaused = false;
  double _downloadProgress = 0;
  String _downloadStatus = '';
  String? _lastHangzhouPath;
  String? _lastLocalPath;
  int _gain = 20;
  TransferController? _transferController;

  List<List<int>> _ledArrayEntries =
      BleProtocol.cloneLedArrayEntries(BleProtocol.defaultLedArrayEntries);

  int _sampleCount = 0;
  int _lastRawLog = 0;
  int _streamPairIdx = 0;
  int? _pending735;
  DateTime? _acqStartedAt;
  Timer? _rateUiTimer;

  int? _boardBytes;
  int? _boardFrames;
  double? _boardSpeedKbps;
  double? _boardFrameRate;
  int? _prevBoardBytes;
  int? _prevBoardFrames;
  DateTime? _prevBoardStatAt;
  bool _boardStatLive = false;
  bool _boardStatPolling = false;

  /// Board Hangzhou.bin write period follows LED step count (11 ms × steps).
  double get _boardSamplePeriodMs =>
      BleProtocol.ledArrayEstimatePeriodMs(_ledArrayEntries).toDouble();

  bool _subBias = false;
  bool _detrend = false;
  bool _filter = false;

  SignalProcOptions get _procOpts => SignalProcOptions(
        subtractBias: _subBias,
        detrend: _detrend,
        filter: _filter,
      );

  void _rebuildPlots() {
    final opts = _procOpts;
    _plots = _displayChannels.map((c) => _ChannelPlot(c, opts)).toList();
  }

  void _resetStreamParse() {
    _streamPairIdx = 0;
    _pending735 = null;
    _sampleCount = 0;
    _lastRawLog = 0;
    _acqStartedAt = null;
    for (final p in _plots) {
      p.reset();
    }
  }

  @override
  void initState() {
    super.initState();
    _rebuildPlots();
  }

  @override
  void dispose() {
    _rateUiTimer?.cancel();
    _transferController?.cancel();
    final liveSync = _liveSync;
    if (liveSync != null) {
      unawaited(liveSync.dispose());
    }
    unawaited(_stream.dispose());
    unawaited(_fnirs.dispose());
    super.dispose();
  }

  void _log(String m) {
    setState(() {
      _logs.insert(0, m);
      if (_logs.length > 30) _logs.removeLast();
    });
  }

  int get _aliveNodeCount {
    final n = _alive.where((e) => e).length;
    return n > 0 ? n : 12;
  }

  /// Hangzhou.bin: (8 B hdr + N×N×96 + N×16) per frame @ sample period.
  void _resetBoardRecordStat() {
    _boardBytes = null;
    _boardFrames = null;
    _boardSpeedKbps = null;
    _boardFrameRate = null;
    _prevBoardBytes = null;
    _prevBoardFrames = null;
    _prevBoardStatAt = null;
    _boardStatLive = false;
    _boardStatPolling = false;
  }

  Future<void> _pollBoardRecordStat() async {
    final dev = widget.device;
    if (dev == null || !_acquiring || _boardStatPolling) return;
    _boardStatPolling = true;
    try {
      final stat = await _fnirs.queryRecordStat(dev, _log);
      final now = DateTime.now();
      if (_prevBoardBytes != null && _prevBoardStatAt != null) {
        final dt = now.difference(_prevBoardStatAt!).inMilliseconds / 1000.0;
        if (dt >= 0.5) {
          final dBytes = stat.bytes - _prevBoardBytes!;
          if (dBytes >= 0) {
            _boardSpeedKbps = (dBytes / dt) / 1024.0;
          }
          if (_prevBoardFrames != null) {
            final dFrames = stat.frames - _prevBoardFrames!;
            if (dFrames >= 0) {
              _boardFrameRate = dFrames / dt;
            }
          }
        }
      }
      _prevBoardBytes = stat.bytes;
      _prevBoardFrames = stat.frames;
      _prevBoardStatAt = now;
      _boardBytes = stat.bytes;
      _boardFrames = stat.frames;
      _boardStatLive = true;
      if (mounted) setState(() {});
    } catch (_) {
      _boardStatLive = false;
      if (mounted) setState(() {});
    } finally {
      _boardStatPolling = false;
    }
  }
  double _estimateBoardRecordKbps(int nodes) {
    if (nodes <= 0) return 0;
    final frameBytes = 8 + nodes * nodes * 96 + nodes * 16;
    return frameBytes * (1000.0 / _boardSamplePeriodMs) / 1024.0;
  }

  String _boardRecordSpeedLabel() {
    if (_boardStatLive && _boardBytes != null) {
      final kb = (_boardBytes! / 1024).toStringAsFixed(1);
      final frames = _boardFrames ?? 0;
      if (_boardSpeedKbps != null && _boardSpeedKbps! > 0) {
        final speed = _boardSpeedKbps!.toStringAsFixed(0);
        if (_boardFrameRate != null && _boardFrameRate! > 0) {
          return '$speed KB/s · 已录 $kb KB · ${_boardFrameRate!.toStringAsFixed(1)} 帧/s';
        }
        return '$speed KB/s · 已录 $kb KB · $frames 帧';
      }
      return '测量中… · 已录 $kb KB · $frames 帧';
    }

    final n = _aliveNodeCount;
    final kbps = _estimateBoardRecordKbps(n);
    final elapsed = _acqStartedAt != null
        ? DateTime.now().difference(_acqStartedAt!).inMilliseconds / 1000.0
        : 0.0;
    final estKb = kbps * elapsed;
    if (_acquiring && elapsed > 0) {
      return '~${kbps.toStringAsFixed(0)} KB/s · 已录约 ${estKb.toStringAsFixed(0)} KB';
    }
    if (_boardBytes != null && _boardBytes! > 0) {
      final kb = (_boardBytes! / 1024).toStringAsFixed(1);
      final frames = _boardFrames ?? 0;
      return '已录 $kb KB · $frames 帧';
    }
    return '~${kbps.toStringAsFixed(0)} KB/s · $n 节点（需刷新固件后实测）';
  }

  String _boardRecordFootnote() {
    if (_boardStatLive) {
      return '板端 Hangzhou.bin 实测写入速度（每秒向板端查询，非 BLE）。';
    }
    return '旧固件无实测接口，显示为理论估算；请刷新 my-fnirs + my-server 后看实测速度。';
  }

  Widget _buildFrameCheckSection(StreamFrameCheck chk) {
    final ok = chk.passed;
    final pending = _acquiring && !chk.hasData;
    final statusColor = pending
        ? Colors.grey.shade700
        : (ok ? Colors.green.shade700 : Colors.orange.shade800);
    final statusIcon = pending
        ? Icons.hourglass_empty
        : (ok ? Icons.verified : Icons.warning_amber_rounded);
    final statusText = pending
        ? '等待首包…'
        : (ok ? '校验通过' : '校验异常');

    String seqVerdict() {
      if (!chk.hasData) return '—';
      if (chk.lostPackets == 0 && chk.outOfOrderPackets == 0) return '连续';
      final parts = <String>[];
      if (chk.lostPackets > 0) parts.add('跳号 ${chk.lostPackets}');
      if (chk.outOfOrderPackets > 0) parts.add('乱序 ${chk.outOfOrderPackets}');
      return parts.join(' · ');
    }

    String sampleVerdict() {
      if (!chk.hasData) return '—';
      if (chk.lostSamples == 0) return '连续';
      return '缺失 ${chk.lostSamples} 点';
    }

    return _section('BLE 帧校验 (STREAM 0x32)', [
      Row(
        children: [
          Icon(statusIcon, color: statusColor, size: 22),
          const SizedBox(width: 8),
          Text(
            statusText,
            style: TextStyle(
              fontWeight: FontWeight.w700,
              color: statusColor,
              fontSize: 15,
            ),
          ),
        ],
      ),
      const SizedBox(height: 8),
      _frameCheckRow(
        '包序号 seq',
        seqVerdict(),
        !chk.hasData || (chk.lostPackets == 0 && chk.outOfOrderPackets == 0),
      ),
      _frameCheckRow(
        '采样索引 sampleBase',
        sampleVerdict(),
        !chk.hasData || chk.lostSamples == 0,
      ),
      _frameCheckRow(
        '已接收',
        chk.hasData ? '${chk.packets} 包 · ${chk.samples} 点' : '—',
        true,
      ),
      if (chk.lastSeq != null)
        _frameCheckRow('末包 seq', '${chk.lastSeq}', true),
      if (chk.tailMissing != null && chk.tailMissing! > 0)
        _frameCheckRow('板端尾缺', '${chk.tailMissing} 点', false),
      const SizedBox(height: 6),
      const Text(
        '协议无 CRC，仅校验 Notify 包 seq 与 sampleBase 连续性。'
        'Hangzhou.bin 完整性以下载 CRC32 为准。',
        style: TextStyle(fontSize: 10, color: Colors.black54),
      ),
    ]);
  }

  Widget _frameCheckRow(String label, String value, bool ok) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 4),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Icon(
            ok ? Icons.check : Icons.close,
            size: 14,
            color: ok ? Colors.green.shade600 : Colors.orange.shade700,
          ),
          const SizedBox(width: 6),
          Expanded(
            child: RichText(
              text: TextSpan(
                style: const TextStyle(fontSize: 12, color: Colors.black87),
                children: [
                  TextSpan(
                    text: '$label: ',
                    style: const TextStyle(fontWeight: FontWeight.w600),
                  ),
                  TextSpan(text: value),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }

  Future<void> _scan() async {
    final dev = widget.device;
    if (dev == null) return;
    setState(() => _busy = true);
    try {
      await _fnirs.prepareLink(dev, _log);
      final alive = await _fnirs.scanNodes(dev, _log);
      setState(() => _alive = alive);
      _log('扫描完成: ${alive.where((e) => e).length} 节点在线');
    } catch (e) {
      _log('扫描失败: $e');
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _openChannelFilter() async {
    final result = await showDialog<FnirsChannelFilterResult>(
      context: context,
      builder: (_) => FnirsChannelFilterDialog(
        initialDisplay: _displayChannels,
        maxDisplay: _maxDisplay,
        maxBlePreview: _maxBlePreview,
      ),
    );
    if (result == null || !mounted) return;

    setState(() {
      _displayChannels = result.displayChannels;
      _blePreviewChannels = result.blePreviewChannels;
      _rebuildPlots();
      _resetStreamParse();
    });
    _log('通道筛选: 显示 ${_displayChannels.length} 路');

    if (_acquiring && widget.device != null) {
      setState(() => _busy = true);
      try {
        await _fnirs.setStreamChannels(
            widget.device!, _blePreviewChannels, _log);
        _log('BLE 预览已切换');
      } catch (e) {
        _log('预览切换失败: $e');
      } finally {
        if (mounted) setState(() => _busy = false);
      }
    }
  }

  Future<void> _applyLedArrayToBoard(BluetoothDevice dev) async {
    await _fnirs.setLedArray(dev, _ledArrayEntries, _log);
    _log('发光序列已下发: ${BleProtocol.ledArraySummary(_ledArrayEntries)}');
  }

  Future<void> _openLedArrayEditor() async {
    final result = await showDialog<List<List<int>>>(
      context: context,
      builder: (_) => FnirsLedArrayDialog(initialEntries: _ledArrayEntries),
    );
    if (result == null || !mounted) return;

    setState(() => _ledArrayEntries = result);
    _log('发光序列: ${BleProtocol.ledArraySummary(_ledArrayEntries)}');

    final dev = widget.device;
    if (dev == null || _acquiring) return;

    setState(() => _busy = true);
    try {
      await _fnirs.prepareLink(dev, _log);
      await _applyLedArrayToBoard(dev);
    } catch (e) {
      _log('发光序列下发失败: $e');
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('发光序列下发失败: $e')),
        );
      }
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _startAcquisition() async {
    final dev = widget.device;
    if (dev == null) return;

    final controller = TransferController();
    final liveSync = BleLiveSyncService();
    _transferController = controller;
    _liveSync = liveSync;
    _stream.bindAuxNotify(liveSync.onNotify);

    setState(() {
      _busy = true;
      _acquiring = true;
      _downloading = true;
      _downloadPaused = false;
      _downloadProgress = 0;
      _downloadStatus = '等待板端创建采集文件…';
      _lastHangzhouPath = null;
      _lastLocalPath = null;
      _rebuildPlots();
      _resetStreamParse();
      _resetBoardRecordStat();
      _acqStartedAt = DateTime.now();
    });
    try {
      await _fnirs.prepareLink(dev, _log);
      _log('MTU=${dev.mtuNow}');

      await _fnirs.setGain(dev, _gain, _log);
      try {
        await _applyLedArrayToBoard(dev);
      } catch (e) {
        _log('LED 阵列跳过: $e');
      }

      await _fnirs.setStreamChannels(dev, _blePreviewChannels, _log);

      await _stream.start(
        device: dev,
        onPacket: _onStreamPacket,
        onLog: _log,
      );
      final syncChar = _stream.linkedChar;
      if (syncChar == null) {
        throw StateError('实时流已启动，但未取得 BLE 传输特征');
      }
      await liveSync.start(
        device: dev,
        characteristic: syncChar,
        controller: controller,
        onLog: _log,
        onProgress: (received, finalSize, rate) {
          if (!mounted || _liveSync != liveSync || _downloadPaused) return;
          setState(() {
            _downloadProgress =
                finalSize != null && finalSize > 0 ? received / finalSize : 0;
            if (finalSize == null) {
              _downloadStatus =
                  '边采边同步 ${(received / 1024).toStringAsFixed(1)} KB  '
                  '(${(rate / 1024).toStringAsFixed(1)} KB/s)';
            } else {
              _downloadStatus =
                  '停止后补齐 ${(received / 1024).toStringAsFixed(1)} / '
                  '${(finalSize / 1024).toStringAsFixed(1)} KB  '
                  '(${(rate / 1024).toStringAsFixed(1)} KB/s)';
            }
          });
        },
      );
      unawaited(_watchLiveSync(liveSync));
      _log(
        '采集已启动 · Hangzhou.bin 边录边同步 · BLE 预览 ${_blePreviewChannels.length} 路',
      );
      _rateUiTimer?.cancel();
      _rateUiTimer = Timer.periodic(const Duration(seconds: 1), (_) {
        if (mounted && _acquiring) {
          unawaited(_pollBoardRecordStat());
          setState(() {});
        }
      });
      unawaited(_pollBoardRecordStat());
    } catch (e) {
      _log('开始采集失败: $e');
      _rateUiTimer?.cancel();
      _rateUiTimer = null;
      try {
        await liveSync.cancel();
      } catch (_) {}
      try {
        await _stream.stop(
          onLog: _log,
          keepLink: true,
          prefetchLast: false,
        );
        await _fnirs.sampleOff(dev, _log);
      } catch (_) {}
      _stream.bindAuxNotify(null);
      if (mounted) {
        setState(() {
          _acquiring = false;
          _downloading = false;
        });
      }
      _transferController = null;
      _liveSync = null;
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _stopAcquisition() async {
    final dev = widget.device;
    if (dev == null) return;

    setState(() => _busy = true);
    var finalized = false;
    var fallbackFullDownload = false;
    try {
      await _pollBoardRecordStat();
      await _stream.stop(
        onLog: _log,
        keepLink: true,
        prefetchLast: false,
      );
      await _fnirs.sampleOff(dev, _log);
      _acquiring = false;
      final liveSync = _liveSync;
      if (liveSync != null && liveSync.isActive) {
        liveSync.expectFinalization();
      } else {
        if (liveSync != null) {
          if (_liveSync == liveSync) {
            _liveSync = null;
            _stream.bindAuxNotify(null);
            _transferController = null;
          }
          await liveSync.dispose();
        }
        fallbackFullDownload = true;
        _log('采集已封口，但 LIVE 会话已结束；准备自动完整下载');
      }
      finalized = true;
      _log(
        fallbackFullDownload
            ? '采集已停止，切换到已封口文件下载'
            : '采集已停止，LIVE 下载继续补齐剩余文件',
      );
      if (mounted && _downloading) {
        setState(() {
          _downloadStatus = fallbackFullDownload
              ? '采集已停止，准备重新下载完整文件…'
              : '采集已停止，等待板端封口；已同步 '
                  '${((liveSync?.receivedBytes ?? 0) / 1024).toStringAsFixed(1)} KB…';
        });
      }
    } catch (e) {
      _log('停止/文件落盘失败: $e');
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('文件尚未就绪，请再次点击停止：$e')),
        );
      }
    } finally {
      if (mounted) {
        _rateUiTimer?.cancel();
        _rateUiTimer = null;
        setState(() {
          _busy = false;
          _acquiring = !finalized;
        });
      }
    }
    if (fallbackFullDownload && mounted) {
      await _downloadHangzhou();
    }
  }

  void _pauseDownload() {
    if (!_downloading || _downloadPaused) return;
    _transferController?.pause();
    setState(() {
      _downloadPaused = true;
      _downloadStatus = '已暂停';
    });
    _log('下载已暂停');
  }

  void _resumeDownload() {
    if (!_downloading || !_downloadPaused) return;
    _transferController?.resume();
    setState(() => _downloadPaused = false);
    _log('继续下载…');
  }

  void _cancelDownload() {
    if (!_downloading) return;
    _transferController?.cancel();
    final liveSync = _liveSync;
    if (liveSync != null && liveSync.isActive) {
      unawaited(liveSync.cancel());
    }
    _log('正在取消下载…');
  }

  Future<void> _watchLiveSync(BleLiveSyncService liveSync) async {
    var retryFullDownload = false;
    try {
      final result = await liveSync.done;
      if (!mounted || _liveSync != liveSync) return;
      setState(() {
        _downloadProgress = 1;
        _downloadStatus = '同步完成';
        _lastHangzhouPath = result.filename;
        _lastLocalPath = result.displayPath;
      });
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text('完整采集文件已保存：${result.displayPath}'),
          backgroundColor: Colors.green,
          duration: const Duration(seconds: 6),
        ),
      );
    } on TransferCancelledException {
      if (mounted) {
        _log('LIVE 下载已取消；停止采集后可手动下载完整文件');
      }
    } catch (e) {
      if (mounted) {
        _log('LIVE 下载失败: $e');
      }
      if (mounted && _liveSync == liveSync) {
        retryFullDownload = !_acquiring;
        if (!retryFullDownload) {
          ScaffoldMessenger.of(context).showSnackBar(
            SnackBar(content: Text('边采边下载失败，停止后可手动重试：$e')),
          );
        }
      }
    } finally {
      if (mounted && _liveSync == liveSync) {
        setState(() {
          _downloading = false;
          _downloadPaused = false;
        });
        _transferController = null;
        _liveSync = null;
        _stream.bindAuxNotify(null);
      }
    }
    if (retryFullDownload && mounted) {
      _log('LIVE 尾包失败，自动改用已封口完整文件下载…');
      await _downloadHangzhou();
    }
  }

  Future<void> _downloadHangzhou() async {
    final selected = widget.device;
    if (selected == null) return;
    if (_acquiring) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('请先停止采集再下载')),
      );
      return;
    }

    final controller = TransferController();
    _transferController = controller;
    setState(() {
      _downloading = true;
      _downloadPaused = false;
      _downloadProgress = 0;
      _downloadStatus = '查询录制路径…';
      _lastLocalPath = null;
    });

    try {
      var dev = selected;
      if (!await BleConnect.isConnected(dev)) {
        _log('BLE 已断开，自动重连后继续完整文件下载…');
        BleTransferService.clearGattCache();
        dev = await BleConnect.connectForTransfer(
          remoteId: selected.remoteId.str,
          preferredDevice: selected,
          log: _log,
          reuseIfConnected: false,
          disconnectAllFirst: false,
        );
      }
      await _fnirs.prepareLink(dev, _log);
      final result = await _rx.downloadHangzhouRecording(
        device: dev,
        queryPath: () => _fnirs.queryHangzhouPath(dev, _log),
        streamService: _stream,
        controller: controller,
        onLog: _log,
        onProgress: (recv, total, rate) {
          if (!mounted || _downloadPaused) return;
          setState(() {
            _downloadProgress = total > 0 ? recv / total : 0;
            _downloadStatus =
                '${(recv / 1024).toStringAsFixed(1)} / ${(total / 1024).toStringAsFixed(1)} KB  '
                '(${(rate / 1024).toStringAsFixed(1)} KB/s)';
          });
        },
      );

      if (mounted) {
        setState(() {
          _downloadProgress = 1;
          _downloadStatus = '下载完成';
          _lastHangzhouPath = result.filename;
          _lastLocalPath = result.displayPath;
        });
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text('已保存 ${result.displayPath}'),
            backgroundColor: Colors.green,
            duration: const Duration(seconds: 6),
          ),
        );
      }
    } on TransferCancelledException {
      _log('下载已取消');
    } catch (e) {
      _log('下载失败: $e');
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('下载失败: $e')),
        );
      }
    } finally {
      if (mounted) {
        setState(() {
          _downloading = false;
          _downloadPaused = false;
        });
      }
      _transferController = null;
    }
  }

  int? _displayIndexForBleIndex(int bleIdx) {
    if (bleIdx < 0 || bleIdx >= _blePreviewChannels.length) return null;
    final ch = _blePreviewChannels[bleIdx];
    for (var i = 0; i < _displayChannels.length; i++) {
      if (FnirsChannelExt.same(_displayChannels[i], ch)) return i;
    }
    return null;
  }

  void _pushSampleToDisplay(int bleIdx, int raw735, int raw850) {
    final di = _displayIndexForBleIndex(bleIdx);
    if (di == null || di >= _plots.length) return;
    _plots[di].pushPair(raw735, raw850);
  }

  void _onStreamPacket(StreamDataPacket pkt) {
    final nBle = _blePreviewChannels.length;
    if (nBle == 0 || pkt.samples.isEmpty) return;

    var i = 0;
    if (_pending735 != null) {
      final bleIdx = _streamPairIdx % nBle;
      _pushSampleToDisplay(bleIdx, _pending735!, pkt.samples[0]);
      _pending735 = null;
      _streamPairIdx++;
      i = 1;
    }

    while (i + 1 < pkt.samples.length) {
      final bleIdx = _streamPairIdx % nBle;
      _pushSampleToDisplay(bleIdx, pkt.samples[i], pkt.samples[i + 1]);
      _streamPairIdx++;
      i += 2;
    }

    if (i < pkt.samples.length) {
      _pending735 = pkt.samples[i];
    }

    if (mounted) {
      setState(() => _sampleCount += pkt.samples.length);
      if (_sampleCount - _lastRawLog >= 80 && pkt.samples.isNotEmpty) {
        _lastRawLog = _sampleCount;
        final uv = pkt.samples.take(3).map((r) => r * 10).join(',');
        _log('光强µV前3: $uv');
      }
    }
  }

  Widget _noDevice() {
    return Center(
      child: Padding(
        padding: const EdgeInsets.all(32),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(Icons.bluetooth_disabled,
                size: 56, color: Colors.grey.shade400),
            const SizedBox(height: 16),
            const Text(
              '请先在「设备」页连接 TestBLE',
              textAlign: TextAlign.center,
              style: TextStyle(fontSize: 15),
            ),
          ],
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    if (widget.device == null) {
      return Scaffold(
        appBar: AppBar(title: const Text('fNIRS 通道监测')),
        body: _noDevice(),
      );
    }

    return Scaffold(
      appBar: AppBar(
        title: const Text('fNIRS 通道监测'),
        actions: [
          if (_acquiring)
            Padding(
              padding: const EdgeInsets.only(right: 12),
              child: Center(
                child: Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Container(
                      width: 8,
                      height: 8,
                      decoration: const BoxDecoration(
                        color: Colors.redAccent,
                        shape: BoxShape.circle,
                      ),
                    ),
                    const SizedBox(width: 6),
                    const Text('REC',
                        style: TextStyle(color: Colors.redAccent)),
                  ],
                ),
              ),
            ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.all(12),
        children: [
          _section('节点状态', [
            Wrap(
              spacing: 6,
              runSpacing: 6,
              children: List.generate(12, (i) {
                final on = _alive[i];
                return Chip(
                  label: Text('${i + 1}', style: const TextStyle(fontSize: 12)),
                  backgroundColor:
                      on ? Colors.green.shade100 : Colors.grey.shade200,
                  visualDensity: VisualDensity.compact,
                );
              }),
            ),
            const SizedBox(height: 8),
            Row(
              children: [
                FilledButton.tonal(
                  onPressed: _busy ? null : _scan,
                  child: const Text('扫描节点'),
                ),
                const SizedBox(width: 8),
                Text('${_alive.where((e) => e).length}/12 在线'),
              ],
            ),
          ]),
          _section('采集控制', [
            const Text(
              '全通道录制 Hangzhou.bin；下方波形为 BLE 预览子集（带宽有限）。',
              style: TextStyle(fontSize: 11, color: Colors.black54),
            ),
            if (_acquiring || (_boardBytes != null && _boardBytes! > 0)) ...[
              const SizedBox(height: 8),
              _metricRow(
                Icons.save_alt,
                '板端录制',
                _boardRecordSpeedLabel(),
              ),
              Padding(
                padding: const EdgeInsets.only(top: 4),
                child: Text(
                  _boardRecordFootnote(),
                  style: const TextStyle(fontSize: 10, color: Colors.black54),
                ),
              ),
            ],
            const SizedBox(height: 8),
            Row(
              children: [
                Expanded(
                  child: FilledButton.icon(
                    onPressed: (_busy || _acquiring || _downloading)
                        ? null
                        : _startAcquisition,
                    icon: const Icon(Icons.play_arrow),
                    label: const Text('开始采集'),
                  ),
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: OutlinedButton.icon(
                    onPressed: (_busy || !_acquiring) ? null : _stopAcquisition,
                    icon: const Icon(Icons.stop),
                    label: const Text('停止'),
                  ),
                ),
              ],
            ),
            const SizedBox(height: 8),
            OutlinedButton.icon(
              onPressed: (_busy || _acquiring) ? null : _openLedArrayEditor,
              icon: const Icon(Icons.light_mode_outlined),
              label: Text('发光序列 (${_ledArrayEntries.length} 步)'),
            ),
            Padding(
              padding: const EdgeInsets.only(top: 4, bottom: 4),
              child: Text(
                BleProtocol.ledArraySummary(_ledArrayEntries),
                style: const TextStyle(fontSize: 10, color: Colors.black54),
              ),
            ),
            Wrap(
              spacing: 4,
              runSpacing: 4,
              children: [
                for (final e in _ledArrayEntries)
                  Chip(
                    label: Text(BleProtocol.ledArrayStepLabel(e),
                        style: const TextStyle(fontSize: 10)),
                    visualDensity: VisualDensity.compact,
                    materialTapTargetSize: MaterialTapTargetSize.shrinkWrap,
                  ),
              ],
            ),
            const SizedBox(height: 8),
            OutlinedButton.icon(
              onPressed: _busy ? null : _openChannelFilter,
              icon: const Icon(Icons.filter_list),
              label: Text('通道筛选 (${_displayChannels.length} 路)'),
            ),
            Padding(
              padding: const EdgeInsets.only(top: 4, bottom: 4),
              child: Wrap(
                spacing: 4,
                runSpacing: 4,
                children: [
                  for (final c in _displayChannels)
                    Chip(
                      label: Text(c.label, style: const TextStyle(fontSize: 10)),
                      visualDensity: VisualDensity.compact,
                      materialTapTargetSize: MaterialTapTargetSize.shrinkWrap,
                    ),
                ],
              ),
            ),
            const SizedBox(height: 8),
            Row(
              children: [
                const Text('增益'),
                Expanded(
                  child: Slider(
                    min: 0,
                    max: 50,
                    divisions: 50,
                    value: _gain.toDouble(),
                    label: '$_gain',
                    onChanged: _busy || _acquiring
                        ? null
                        : (v) => setState(() => _gain = v.round()),
                  ),
                ),
              ],
            ),
          ]),
          if (_acquiring || _stream.frameCheck.hasData)
            _buildFrameCheckSection(_stream.frameCheck),
          _section('信号处理', [
            SwitchListTile(
              dense: true,
              contentPadding: EdgeInsets.zero,
              title: const Text('减去偏置 100000 μV', style: TextStyle(fontSize: 13)),
              value: _subBias,
              onChanged: _acquiring
                  ? null
                  : (v) => setState(() {
                        _subBias = v;
                        _rebuildPlots();
                      }),
            ),
            SwitchListTile(
              dense: true,
              contentPadding: EdgeInsets.zero,
              title: const Text('去趋势', style: TextStyle(fontSize: 13)),
              value: _detrend,
              onChanged: _acquiring
                  ? null
                  : (v) => setState(() {
                        _detrend = v;
                        _rebuildPlots();
                      }),
            ),
            SwitchListTile(
              dense: true,
              contentPadding: EdgeInsets.zero,
              title: const Text('滤波 0.01–2 Hz', style: TextStyle(fontSize: 13)),
              value: _filter,
              onChanged: _acquiring
                  ? null
                  : (v) => setState(() {
                        _filter = v;
                        _rebuildPlots();
                      }),
            ),
          ]),
          _section('实时波形 (${_displayChannels.length} 路 × 735/850)', [
            Text(
              _acquiring
                  ? 'BLE 预览 · $_sampleCount 点 · μV'
                  : '未采集',
              style: const TextStyle(fontSize: 12),
            ),
            const SizedBox(height: 8),
            ...List.generate(_plots.length, (i) {
              return Card(
                margin: const EdgeInsets.only(bottom: 10),
                child: Padding(
                  padding: const EdgeInsets.all(10),
                  child: DualWaveformChart(
                    title: _plots[i].ch.label,
                    samples735: _plots[i].w735,
                    samples850: _plots[i].w850,
                    isLive: _acquiring,
                  ),
                ),
              );
            }),
          ]),
          _section('完整文件同步', [
            const Text(
              '采集时同步完整文件；停止后自动补齐尾部并校验 CRC32。保存：下载/TestBLE/',
              style: TextStyle(fontSize: 11, color: Colors.black54),
            ),
            if (_downloading) ...[
              const SizedBox(height: 10),
              LinearProgressIndicator(
                value: _acquiring
                    ? null
                    : _downloadProgress.clamp(0.0, 1.0),
              ),
              const SizedBox(height: 6),
              Text(_downloadStatus,
                  style: const TextStyle(fontSize: 11, fontFamily: 'monospace')),
            ],
            if (_lastLocalPath != null) ...[
              const SizedBox(height: 8),
              Text('已下载: $_lastHangzhouPath\n$_lastLocalPath',
                  style: const TextStyle(fontSize: 11)),
            ],
            const SizedBox(height: 10),
            if (!_downloading)
              SizedBox(
                width: double.infinity,
                child: FilledButton.icon(
                  onPressed: (_busy || _acquiring) ? null : _downloadHangzhou,
                  icon: const Icon(Icons.cloud_download),
                  label: const Text('停止后手动重试下载'),
                ),
              )
            else if (_downloadPaused)
              Row(
                children: [
                  Expanded(
                    child: FilledButton.icon(
                      onPressed: _resumeDownload,
                      icon: const Icon(Icons.play_arrow),
                      label: const Text('继续'),
                    ),
                  ),
                  const SizedBox(width: 8),
                  Expanded(
                    child: OutlinedButton.icon(
                      onPressed: _cancelDownload,
                      icon: const Icon(Icons.close),
                      label: const Text('取消'),
                    ),
                  ),
                ],
              )
            else
              Row(
                children: [
                  Expanded(
                    flex: 2,
                    child: FilledButton.icon(
                      onPressed: null,
                      icon: const Icon(Icons.cloud_download),
                      label: const Text('下载中…'),
                    ),
                  ),
                  const SizedBox(width: 8),
                  Expanded(
                    child: OutlinedButton.icon(
                      onPressed: _pauseDownload,
                      icon: const Icon(Icons.pause),
                      label: const Text('暂停'),
                    ),
                  ),
                ],
              ),
          ]),
          if (_logs.isNotEmpty)
            _section('日志', [
              Text(
                _logs.take(6).join('\n'),
                style: const TextStyle(fontSize: 11, fontFamily: 'monospace'),
              ),
            ]),
        ],
      ),
    );
  }

  Widget _metricRow(IconData icon, String label, String value) {
    return Row(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Icon(icon, size: 18, color: Theme.of(context).colorScheme.primary),
        const SizedBox(width: 8),
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(label,
                  style: const TextStyle(
                      fontSize: 11, fontWeight: FontWeight.w600)),
              Text(value,
                  style: const TextStyle(
                      fontSize: 12, fontFamily: 'monospace')),
            ],
          ),
        ),
      ],
    );
  }

  Widget _section(String title, List<Widget> children) {
    return Card(
      margin: const EdgeInsets.only(bottom: 12),
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(title, style: const TextStyle(fontWeight: FontWeight.w700)),
            const SizedBox(height: 8),
            ...children,
          ],
        ),
      ),
    );
  }
}
