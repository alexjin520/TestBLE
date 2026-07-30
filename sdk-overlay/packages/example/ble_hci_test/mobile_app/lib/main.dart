import 'dart:async';
import 'dart:io';
import 'dart:math' as math;

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'ble_transfer.dart';
import 'ble_connect.dart';
import 'ble_board_rx.dart';
import 'ble_stream.dart';
import 'fnirs_monitor_page.dart';
import 'protocol.dart';
import 'waveform_chart.dart';

class _ManualMacPick {
  const _ManualMacPick();
}

void main() {
  FlutterBluePlus.setLogLevel(LogLevel.warning);
  runApp(const TestBleApp());
}

class TestBleApp extends StatelessWidget {
  const TestBleApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'fNIRS BLE Demo',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF00695C),
          brightness: Brightness.light,
        ),
        useMaterial3: true,
        cardTheme: CardThemeData(
          elevation: 0,
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(16),
            side: BorderSide(color: Colors.grey.shade300),
          ),
        ),
        filledButtonTheme: FilledButtonThemeData(
          style: FilledButton.styleFrom(
            padding: const EdgeInsets.symmetric(vertical: 14, horizontal: 20),
            shape: RoundedRectangleBorder(
              borderRadius: BorderRadius.circular(12),
            ),
          ),
        ),
      ),
      home: const HomePage(),
    );
  }
}

class HomePage extends StatefulWidget {
  const HomePage({super.key});

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  static const _prefDeviceId = 'last_ble_device_id';

  int _tabIndex = 0;
  final _logs = <String>[];
  BluetoothDevice? _device;
  String? _rememberedDeviceId;
  String? _filePath;
  bool _scanning = false;
  bool _sending = false;
  bool _downloading = false;
  bool _streaming = false;
  bool _streamStarting = false;
  bool _disconnecting = false;
  bool _paused = false;
  bool _frameCrc = true;
  bool _fastMode = true;
  double _progress = 0;
  String _status = '就绪';
  _SendSummary? _lastSummary;
  _ReceiveSummary? _lastReceive;
  final _downloadNameCtrl = TextEditingController();
  TransferController? _transferController;
  final BleStreamService _streamService = BleStreamService();
  final List<double> _waveSamples = [];
  final List<int> _decimBuffer = [];
  static const _waveDisplayHz = 50;
  static const _waveCapacity = 1000;
  static const _waveDisplayScale = 800.0;
  static const _waveHpAlpha = 0.012;
  int _streamPacketCount = 0;
  int _streamSampleCount = 0;
  int _streamRateHz = BleProtocol.defaultStreamRateHz;
  double? _hpEma;
  DateTime? _streamStartedAt;
  double? _streamRateSnapshotBps;
  int? _rssi;
  Timer? _rssiTimer;
  StreamSubscription<List<ScanResult>>? _rssiScanSub;
  bool _rssiPolling = false;
  bool _gattLinked = false;

  @override
  void initState() {
    super.initState();
    _restoreDevice();
  }

  @override
  void dispose() {
    _stopRssiMonitor();
    _downloadNameCtrl.dispose();
    unawaited(_streamService.dispose());
    unawaited(_disconnectDevice());
    super.dispose();
  }

  bool get _busy =>
      _sending ||
      _downloading ||
      _streaming ||
      _streamStarting ||
      _disconnecting;

  Future<void> _disconnectDevice() async {
    final d = _device;
    if (d == null) return;
    try {
      if (await BleConnect.isConnected(d)) {
        await BleConnect.disconnectAndWait(d, clearGattCache: false);
      }
    } catch (_) {}
  }

  String _rssiLabel(int rssi) {
    if (rssi >= -55) return '强';
    if (rssi >= -70) return '中';
    return '弱';
  }

  Color _rssiColor(int? rssi) {
    if (rssi == null) return Colors.grey;
    if (rssi >= -55) return Colors.green;
    if (rssi >= -70) return Colors.orange;
    return Colors.red;
  }

  void _startRssiMonitor() {
    _stopRssiMonitor();
    if (_device == null || _tabIndex != 0) return;
    unawaited(_pollRssi());
    _rssiTimer = Timer.periodic(const Duration(seconds: 3), (_) {
      unawaited(_pollRssi());
    });
  }

  void _stopRssiMonitor() {
    _rssiTimer?.cancel();
    _rssiTimer = null;
    unawaited(_stopRssiScan());
  }

  /// 发送前必须停干净，否则会和 ble_connect 的扫描冲突。
  Future<void> _stopRssiScan() async {
    await _rssiScanSub?.cancel();
    _rssiScanSub = null;
    try {
      await FlutterBluePlus.stopScan();
    } catch (_) {}
    while (_rssiPolling) {
      await Future<void>.delayed(const Duration(milliseconds: 50));
    }
  }

  Future<void> _pollRssi() async {
    if (_device == null ||
        _tabIndex != 0 ||
        _scanning ||
        _busy ||
        _rssiPolling ||
        !mounted) {
      return;
    }
    _rssiPolling = true;
    final targetId = _device!.remoteId.str.toUpperCase();

    try {
      // Never BLE-scan while GATT is up — Android drops discover/notify.
      if (_gattLinked && await BleConnect.isConnected(_device!)) {
        try {
          final rssi = await _device!.readRssi();
          if (mounted) setState(() => _rssi = rssi);
        } catch (_) {}
        return;
      }

      int? latest;
      await _rssiScanSub?.cancel();
      _rssiScanSub = FlutterBluePlus.onScanResults.listen((results) {
        for (final r in results) {
          if (r.device.remoteId.str.toUpperCase() == targetId) {
            latest = r.rssi;
          }
        }
      });

      await bleStartTestBleScan(
        timeout: const Duration(seconds: 2),
        withRemoteIds: [targetId],
      );
      await Future<void>.delayed(const Duration(seconds: 2));
      await FlutterBluePlus.stopScan();
      await _rssiScanSub?.cancel();
      _rssiScanSub = null;

      if (latest != null && mounted) {
        setState(() => _rssi = latest);
      }
    } catch (_) {
      // Ignore background RSSI poll errors.
    } finally {
      _rssiPolling = false;
    }
  }

  void _markGattLinked(bool linked) {
    _gattLinked = linked;
  }

  void _dropGattLink() {
    _gattLinked = false;
    BleTransferService.clearGattCache();
    _streamService.releaseLinkAfterTransfer();
  }

  /// After FILE TX: keep link + cache so the next STREAM skips discover.
  Future<void> _recoverGattAfterDownload() async {
    _stopRssiMonitor();
    await _stopRssiScan();
    var d = _device;
    if (d == null) return;

    if (!await BleConnect.isConnected(d)) {
      _log('下载后连接已断，重新预热…');
      try {
        d = await _ensureGattWarm(d);
      } catch (e) {
        _log('下载后重连失败: $e');
        _dropGattLink();
        return;
      }
    }

    final id = d.remoteId.str;
    var cached = BleTransferService.cachedGatt(id);
    if (cached == null) {
      _log('下载后无 GATT 缓存，尝试补 discover…');
      try {
        d = await _ensureGattWarm(d);
        cached = BleTransferService.cachedGatt(id);
      } catch (e) {
        _log('补缓存失败: $e');
        _dropGattLink();
        return;
      }
    }
    if (cached == null) {
      _dropGattLink();
      return;
    }

    BleTransferService.resetGattChain();
    await BleTransferService.drainGattChain();
    await Future<void>.delayed(
      Duration(milliseconds: Platform.isAndroid ? 500 : 250),
    );

    try {
      await BleTransferService.resetBoardProtocol(cached.char, _log);
      await BleTransferService.prepareStreamNotify(cached.char, _log);
      _streamService.bindCachedLink(d, cached.char, _log);
      if (mounted) setState(() => _device = d);
      _markGattLinked(true);
      _log('下载完成，连接已就绪，可再次开始实时波形');
    } catch (e) {
      _log('下载后恢复连接失败: $e');
      _dropGattLink();
    }
  }

  Future<BluetoothDevice> _ensureGattWarm(BluetoothDevice device) async {
    final id = device.remoteId.str;
    if (_gattLinked &&
        BleTransferService.hasCachedGatt(id) &&
        await BleConnect.isConnected(device)) {
      _log('复用已预热的 GATT 连接');
      return device;
    }
    if (_streamService.linkedDevice != null &&
        _streamService.linkedChar != null &&
        await BleConnect.isConnected(_streamService.linkedDevice!)) {
      _log('复用 STREAM 已绑定连接');
      _markGattLinked(true);
      return _streamService.linkedDevice!;
    }
    _log('连接板子并缓存 GATT…');
    _stopRssiMonitor();
    await _stopRssiScan();

    Object? lastErr;
    for (var attempt = 1; attempt <= 3; attempt++) {
      try {
        if (attempt > 1) {
          _log('GATT 预热重试 $attempt/3…');
          await Future<void>.delayed(Duration(seconds: attempt));
        }
        final linked = await BleTransferService.warmupGatt(device, _log);
        if (mounted) {
          setState(() => _device = linked);
          _markGattLinked(true);
        }
        return linked;
      } catch (e) {
        lastErr = e;
        _log('GATT 预热失败 $attempt/3: $e');
      }
    }
    throw StateError('GATT 预热失败: $lastErr');
  }

  Future<void> _restoreDevice() async {
    final prefs = await SharedPreferences.getInstance();
    final id = prefs.getString(_prefDeviceId);
    if (id == null) return;
    setState(() {
      _rememberedDeviceId = id;
      _device = BluetoothDevice.fromId(id);
    });
    _log('已记住 MAC: $id（发送前仍会重新扫描）');
    _startRssiMonitor();
  }

  Future<void> _saveDevice(BluetoothDevice d) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_prefDeviceId, d.remoteId.str);
    if (mounted) {
      setState(() => _rememberedDeviceId = d.remoteId.str);
    }
  }

  Future<bool> _confirmDeviceSwitch(String newId) async {
    final remembered = _rememberedDeviceId;
    if (remembered == null || remembered.toUpperCase() == newId.toUpperCase()) {
      return true;
    }
    return await showDialog<bool>(
          context: context,
          builder: (ctx) => AlertDialog(
            title: const Text('切换常用设备？'),
            content: Text(
              '当前记住的是\n$remembered\n\n'
              '确定连接并改为\n$newId ？',
            ),
            actions: [
              TextButton(
                onPressed: () => Navigator.pop(ctx, false),
                child: const Text('取消'),
              ),
              FilledButton(
                onPressed: () => Navigator.pop(ctx, true),
                child: const Text('确认切换'),
              ),
            ],
          ),
        ) ??
        false;
  }

  Future<void> _disconnectFromUi({bool forget = false}) async {
    if (_busy) return;
    final d = _device;
    setState(() {
      _disconnecting = true;
      _status = '正在完整断开蓝牙连接…';
    });
    _stopRssiMonitor();
    try {
      await _stopRssiScan();
      await _streamService.stop(onLog: _log, keepLink: false);
      await BleTransferService.drainGattChain();
      BleTransferService.resetGattChain();
      _dropGattLink();
      if (d != null) {
        await BleConnect.disconnectAndWait(d, log: _log);
      }
      if (forget) {
        final prefs = await SharedPreferences.getInstance();
        await prefs.remove(_prefDeviceId);
      }
      if (!mounted) return;
      setState(() {
        _device = null;
        _rssi = null;
        if (forget) _rememberedDeviceId = null;
        _status = forget ? '已断开并忘记设备' : '已断开连接';
      });
      _log(forget
          ? '已断开并忘记设备，下次不会自动选中'
          : '蓝牙会话已完整释放，可重新扫描连接（仍保留常用设备）');
    } catch (e) {
      _dropGattLink();
      _log('断开清理异常（已强制清除本地会话）: $e');
      if (mounted) {
        setState(() {
          _device = null;
          _rssi = null;
          _status = '连接已清理，可重新扫描';
        });
      }
    } finally {
      if (mounted) setState(() => _disconnecting = false);
    }
  }

  void _log(String msg) {
    setState(() => _logs.add(msg));
  }

  Future<ScanResult?> _pickScanResult(List<ScanResult> candidates) async {
    if (candidates.isEmpty) return null;
    if (candidates.length == 1) return candidates.first;
    return showDialog<ScanResult>(
      context: context,
      builder: (ctx) => SimpleDialog(
        title: const Text('选择板子'),
        children: candidates
            .map(
              (r) => SimpleDialogOption(
                onPressed: () => Navigator.pop(ctx, r),
                child: Text(bleScanResultLabel(r)),
              ),
            )
            .toList(),
      ),
    );
  }

  Future<Object?> _pickFromAllNearby(List<ScanResult> all) async {
    if (all.isEmpty) return null;
    final limited = all.take(40).toList();
    return showDialog<Object>(
      context: context,
      builder: (ctx) => SimpleDialog(
        title: const Text('选择附近设备'),
        children: [
          const Padding(
            padding: EdgeInsets.fromLTRB(24, 0, 24, 8),
            child: Text(
              '未自动匹配 TestBLE，请从列表选手动连接（nRF 能看到的设备应在此）',
              style: TextStyle(fontSize: 13),
            ),
          ),
          ...limited.map(
            (r) => SimpleDialogOption(
              onPressed: () => Navigator.pop(ctx, r),
              child: Text(bleScanResultLabel(r)),
            ),
          ),
          SimpleDialogOption(
            onPressed: () => Navigator.pop(ctx, const _ManualMacPick()),
            child: const Text('手动输入 MAC 地址…'),
          ),
        ],
      ),
    );
  }

  Future<String?> _promptManualMac() async {
    return showDialog<String>(
      context: context,
      builder: (ctx) {
        final ctrl = TextEditingController(text: BleProtocol.defaultBoardMac);
        return AlertDialog(
          title: const Text('手动输入板子 MAC'),
          content: TextField(
            controller: ctrl,
            decoration: const InputDecoration(
              hintText: 'AA:BB:CC:DD:EE:FF',
              border: OutlineInputBorder(),
            ),
            autocorrect: false,
            enableSuggestions: false,
          ),
          actions: [
            TextButton(
              onPressed: () => Navigator.pop(ctx),
              child: const Text('取消'),
            ),
            FilledButton(
              onPressed: () => Navigator.pop(ctx, ctrl.text.trim()),
              child: const Text('确定'),
            ),
          ],
        );
      },
    );
  }

  Future<void> _connectPickedDevice(ScanResult pick) async {
    if (!await _confirmDeviceSwitch(pick.device.remoteId.str)) return;
    final previous = _device;
    setState(() {
      _device = pick.device;
      _rssi = pick.rssi;
    });
    if (previous != null && previous.remoteId != pick.device.remoteId) {
      _markGattLinked(false);
      BleTransferService.clearGattCache();
      await BleConnect.disconnectAndWait(previous, log: _log);
      _log('已切换板子，断开旧连接');
    }
    final pickName = bleScanAdvertisedName(pick);
    _log(
      '已选: ${pickName.isEmpty ? BleProtocol.defaultName : pickName} '
      '${pick.device.remoteId.str} (RSSI ${pick.rssi})',
    );
    setState(() => _status = '连接板子并缓存 GATT…');
    try {
      final linked = await _ensureGattWarm(pick.device);
      // Only a verified GATT connection may replace the remembered device.
      await _saveDevice(linked);
      _log('板子已连接，可直接开始实时波形或下载');
    } catch (e) {
      _markGattLinked(false);
      BleTransferService.clearGattCache();
      if (mounted) {
        setState(() {
          _device = previous;
          _rssi = null;
        });
      }
      _log('预连接失败: $e');
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text('连接板子失败: $e\n请靠近板子后重新扫描'),
            duration: const Duration(seconds: 8),
          ),
        );
      }
    }
    _startRssiMonitor();
  }

  Future<void> _scan() async {
    if (_scanning || _busy) return;
    setState(() {
      _scanning = true;
      _status = '扫描中…';
      _logs.clear();
    });
    _stopRssiMonitor();

    try {
      await bleEnsureScanReady(log: _log);

      final knownId = _rememberedDeviceId ?? _device?.remoteId.str;
      if (knownId != null) {
        _log('已记住 MAC: $knownId（本次仍全量扫描，不用 MAC 过滤）');
      }

      final all = await bleRunDiscoveryScan(duration: const Duration(seconds: 12), log: _log);
      final found = all.where((r) => bleScanIsTestBle(r, knownRemoteId: knownId)).toList();

      ScanResult? pick;
      if (found.isNotEmpty) {
        found.sort((a, b) => b.rssi.compareTo(a.rssi));
        pick = await _pickScanResult(found);
      } else {
        _log('未自动匹配 ${BleProtocol.defaultName}，显示附近全部设备…');
        final choice = await _pickFromAllNearby(all);
        if (choice is _ManualMacPick) {
          final mac = await _promptManualMac();
          if (mac == null || mac.isEmpty) return;
          final target = mac.toUpperCase();
          for (final r in all) {
            if (r.device.remoteId.str.toUpperCase() == target) {
              pick = r;
              break;
            }
          }
          if (pick == null) {
            if (!await _confirmDeviceSwitch(mac)) return;
            _log('列表中未见 $mac，尝试扫描后直连…');
            final linked = await BleConnect.scanAndConnect(
              remoteId: mac,
              log: _log,
              scanTimeout: const Duration(seconds: 8),
              scanRounds: 2,
            );
            setState(() => _device = linked);
            await _ensureGattWarm(linked);
            await _saveDevice(linked);
            _log('手动 MAC 已连接');
            _startRssiMonitor();
            return;
          }
        } else if (choice is ScanResult) {
          pick = choice;
        }
      }

      if (pick == null) {
        if (all.isEmpty) {
          _log('未发现任何 BLE 设备 — 请确认蓝牙已开、板子 my-server 在跑');
          if (mounted) {
            ScaffoldMessenger.of(context).showSnackBar(
              const SnackBar(content: Text('未发现任何 BLE 设备，请检查权限与板子广播')),
            );
          }
        }
        return;
      }

      await _connectPickedDevice(pick);
    } catch (e) {
      _log('扫描失败: $e');
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('扫描失败: $e')),
        );
      }
    } finally {
      if (mounted) {
        setState(() {
          _scanning = false;
          _status = '就绪';
        });
        if (_device != null) _startRssiMonitor();
      }
    }
  }

  Future<void> _pickFile() async {
    if (_busy) return;
    final result = await FilePicker.platform.pickFiles();
    if (result == null || result.files.single.path == null) return;
    setState(() => _filePath = result.files.single.path);
    _log('文件: ${result.files.single.name}');
  }

  void _pauseTransfer() {
    if (!_busy || _paused) return;
    _transferController?.pause();
    setState(() {
      _paused = true;
      _status = '已暂停';
    });
    _log('传输已暂停');
  }

  void _resumeTransfer() {
    if (!_sending || !_paused) return;
    _transferController?.resume();
    setState(() => _paused = false);
    _log('继续传输…');
  }

  void _cancelTransfer() {
    if (!_busy) return;
    _transferController?.cancel();
    _log('正在取消…');
  }

  Future<void> _send() async {
    if (_busy || _device == null || _filePath == null) {
      if (_device == null) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('请先扫描并选择 TestBLE')),
        );
      } else if (_filePath == null) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('请先选择文件')),
        );
      }
      return;
    }

    setState(() {
      _sending = true;
      _paused = false;
      _progress = 0;
      _status = '发送中…';
      _lastSummary = null;
      _lastReceive = null;
    });
    _stopRssiMonitor();
    await _stopRssiScan();
    _log('--- 开始传输 (${BleProtocol.appRev}) ---');

    final file = File(_filePath!);
    final fileBytes = await file.readAsBytes();
    final fileName = file.uri.pathSegments.last;
    final fileCrc = BleProtocol.crc32Ieee(fileBytes);
    final fileSize = fileBytes.length;

    final controller = TransferController();
    _transferController = controller;

    final fc = BleProtocol.fcParams(fast: _fastMode);
    final svc = BleTransferService(
      fastMode: _fastMode,
      chunkSizeOverride: _fastMode ? BleProtocol.fastChunkDefault : null,
      flowControlWindowPkts: fc.window,
      flowControlWaitStepPkts: fc.step,
    );

    final t0 = DateTime.now();
    try {
      await svc.sendFile(
        device: _device!,
        filePath: _filePath!,
        frameCrc: _frameCrc,
        controller: controller,
        reuseConnection: true,
        disconnectAfter: false,
        onLog: _log,
        onProgress: (sent, total, rate) {
          if (!mounted) return;
          setState(() {
            _progress = total > 0 ? sent / total : 0;
            _status =
                '${(sent / 1024).toStringAsFixed(1)} / ${(total / 1024).toStringAsFixed(1)} KB  '
                '(${(rate / 1024).toStringAsFixed(1)} KB/s)';
          });
        },
      );
      final elapsed = DateTime.now().difference(t0).inMilliseconds / 1000.0;
      final kbps = elapsed > 0 ? fileSize / 1024 / elapsed : 0.0;
      if (mounted) {
        setState(() {
          _progress = 1;
          _status = '发送成功';
          _lastSummary = _SendSummary(
            filename: fileName,
            fileSize: fileSize,
            fileCrc: fileCrc,
            kbps: kbps,
          );
        });
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(
            content: Text('发送成功！文件在板子 /app_data/ble_rx/'),
            backgroundColor: Colors.green,
          ),
        );
      }
    } on TransferCancelledException {
      _log('传输已取消');
      if (mounted) {
        setState(() => _status = '已取消');
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('传输已取消')),
        );
      }
    } catch (e) {
      _log('错误: $e');
      if (mounted) {
        setState(() => _status = '失败');
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('发送失败: $e')),
        );
      }
    } finally {
      if (mounted) {
        setState(() {
          _sending = false;
          _paused = false;
        });
        _transferController = null;
        if (_device != null) _startRssiMonitor();
      }
    }
  }

  Future<void> _download() async {
    if (_busy || _device == null) {
      if (_device == null) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('请先扫描并选择 TestBLE')),
        );
      }
      return;
    }

    final remoteName = _downloadNameCtrl.text.trim();
    if (remoteName.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('请输入板子上的文件名')),
      );
      return;
    }

    setState(() {
      _downloading = true;
      _paused = false;
      _progress = 0;
      _status = '下载中…';
      _lastReceive = null;
      _lastSummary = null;
    });
    _stopRssiMonitor();
    await _stopRssiScan();
    _log('--- 从板子下载 (${BleProtocol.appRev}) ---');
    _log('板子路径: ${BleProtocol.bleTxDir}/$remoteName '
        '或 ${BleProtocol.bleRxDir}/$remoteName');

    final controller = TransferController();
    _transferController = controller;
    final svc = BleBoardRxService(frameCrc: _frameCrc);
    final t0 = DateTime.now();
    var downloadOk = false;

    try {
      final result = await svc.receiveFile(
        device: _device!,
        remoteFilename: remoteName,
        controller: controller,
        reuseConnection: true,
        disconnectAfter: false,
        onLog: _log,
        onProgress: (recv, total, rate) {
          if (!mounted) return;
          setState(() {
            _progress = total > 0 ? recv / total : 0;
            _status =
                '${(recv / 1024).toStringAsFixed(1)} / ${(total / 1024).toStringAsFixed(1)} KB  '
                '(${(rate / 1024).toStringAsFixed(1)} KB/s)';
          });
        },
      );
      downloadOk = true;
      final elapsed = DateTime.now().difference(t0).inMilliseconds / 1000.0;
      final kbps = elapsed > 0 ? result.size / 1024 / elapsed : 0.0;
      SimSignalInfo? simInfo;
      if (remoteName.endsWith('.bin')) {
        try {
          final bytes = await File(result.localPath).readAsBytes();
          simInfo = SimSignalInfo.tryParseBytes(bytes);
          if (simInfo != null) {
            _log('fSIM: ${simInfo.demoSummary}');
          }
        } catch (e) {
          _log('本地解析失败: $e');
        }
      }
      if (mounted) {
        setState(() {
          _progress = 1;
          _status = '下载成功';
          _lastReceive = _ReceiveSummary(
            filename: result.filename,
            fileSize: result.size,
            fileCrc: result.crc32,
            localPath: result.localPath,
            displayPath: result.displayPath,
            publicVisible: result.publicVisible,
            kbps: kbps,
            simInfo: simInfo,
          );
        });
        final hint = result.publicVisible
            ? (simInfo != null
                ? '下载完成\n${simInfo.demoSummary}\n${result.localPath}'
                : '已保存\n${result.localPath}')
            : '已保存到 App 内部\n${result.localPath}';
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(hint),
            backgroundColor: result.publicVisible ? Colors.green : Colors.orange,
            duration: const Duration(seconds: 8),
          ),
        );
      }
    } on TransferCancelledException {
      _log('下载已取消');
      if (mounted) {
        setState(() => _status = '已取消');
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('下载已取消')),
        );
      }
    } catch (e) {
      _log('错误: $e');
      if (mounted) {
        setState(() => _status = '下载失败');
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('下载失败: $e')),
        );
      }
    } finally {
      if (downloadOk) {
        await _recoverGattAfterDownload();
      } else {
        _dropGattLink();
      }
      if (mounted) {
        setState(() {
          _downloading = false;
          _paused = false;
        });
        _transferController = null;
        if (_gattLinked) _startRssiMonitor();
      }
    }
  }

  Future<void> _downloadLastRecording() async {
    if (_busy || _device == null) {
      if (_device == null) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('请先扫描并连接 TestBLE')),
        );
      }
      return;
    }

    setState(() {
      _downloading = true;
      _paused = false;
      _progress = 0;
      _status = '查询并下载最近录制…';
      _lastReceive = null;
      _lastSummary = null;
    });
    _stopRssiMonitor();
    await _stopRssiScan();
    _log('--- 下载最近录制 (${BleProtocol.appRev}) ---');
    _log('板子 ble_tx/.last → FILE TX');

    if (_streaming) {
      await _streamService.stop(onLog: _log, keepLink: true);
      if (mounted) {
        setState(() {
          _streaming = false;
          _freezeStreamRate();
        });
      }
    }
    await _streamService.detachNotifyForTransfer(_log);

    final controller = TransferController();
    _transferController = controller;
    final svc = BleBoardRxService(frameCrc: _frameCrc);
    final t0 = DateTime.now();
    var downloadOk = false;

    try {
      final dev = await _ensureGattWarm(_device!);
      _log('FILE TX ACK 开窗=${BleProtocol.txFcWindowPkts}（稳定版 ~14 KB/s）');
      final result = await svc.downloadLastRecording(
        device: dev,
        streamService: _streamService,
        controller: controller,
        onLog: _log,
        onProgress: (recv, total, rate) {
          if (!mounted) return;
          setState(() {
            _progress = total > 0 ? recv / total : 0;
            _status =
                'FILE TX ${(recv / 1024).toStringAsFixed(1)} / ${(total / 1024).toStringAsFixed(1)} KB  '
                '(${(rate / 1024).toStringAsFixed(1)} KB/s)';
          });
        },
      );
      downloadOk = true;
      final elapsed = DateTime.now().difference(t0).inMilliseconds / 1000.0;
      final kbps = elapsed > 0 ? result.size / 1024 / elapsed : 0.0;
      SimSignalInfo? simInfo;
      if (result.filename.endsWith('.bin')) {
        try {
          final bytes = await File(result.localPath).readAsBytes();
          simInfo = SimSignalInfo.tryParseBytes(bytes);
        } catch (_) {}
      }
      if (mounted) {
        setState(() {
          _progress = 1;
          _status = '最近录制下载完成';
          _downloadNameCtrl.text = result.filename;
          _lastReceive = _ReceiveSummary(
            filename: result.filename,
            fileSize: result.size,
            fileCrc: result.crc32,
            localPath: result.localPath,
            displayPath: result.displayPath,
            publicVisible: result.publicVisible,
            kbps: kbps,
            simInfo: simInfo,
          );
        });
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(
              '已从板子下载\n${result.filename}\n${result.localPath}'
              '${result.filename.contains('19700101') || result.filename.contains('sess') ? '\n（板子系统时间未同步）' : ''}',
            ),
            backgroundColor: Colors.green,
            duration: const Duration(seconds: 8),
          ),
        );
      }
    } on TransferCancelledException {
      if (mounted) setState(() => _status = '已取消');
    } catch (e) {
      _log('错误: $e');
      if (mounted) {
        setState(() => _status = '下载失败');
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('下载失败: $e')),
        );
      }
    } finally {
      if (downloadOk) {
        await _recoverGattAfterDownload();
      } else {
        _dropGattLink();
      }
      if (mounted) {
        setState(() {
          _downloading = false;
          _paused = false;
        });
        _transferController = null;
        if (_gattLinked) _startRssiMonitor();
      }
    }
  }

  void _appendWaveSamples(List<int> raw) {
    final block = math.max(1, (_streamRateHz / _waveDisplayHz).round());

    for (final s in raw) {
      _decimBuffer.add(s);
      if (_decimBuffer.length < block) continue;

      var sum = 0.0;
      for (final v in _decimBuffer) {
        sum += v;
      }
      final mean = sum / _decimBuffer.length;
      _decimBuffer.clear();

      // High-pass: remove slow baseline so cardiac (~1 Hz) / resp (~0.3 Hz) show up.
      _hpEma = _hpEma == null
          ? mean
          : (_hpEma! * (1.0 - _waveHpAlpha) + mean * _waveHpAlpha);
      final ac = mean - _hpEma!;

      final norm = (ac / _waveDisplayScale).clamp(-1.0, 1.0);
      _waveSamples.add(norm);
      if (_waveSamples.length > _waveCapacity) {
        _waveSamples.removeAt(0);
      }
    }
  }

  double get _waveWindowSec =>
      _waveSamples.isEmpty ? 0 : _waveSamples.length / _waveDisplayHz;

  String _streamLossLabel() {
    final lostS = _streamService.lostSamples;
    final lostP = _streamService.lostPackets;
    if (lostS == 0 && lostP == 0) return ' · 无丢包';
    return ' · 丢 $lostS 点/$lostP 包';
  }

  String _streamThroughputLabel() {
    final t0 = _streamStartedAt;
    if (t0 == null || _streamSampleCount <= 0) return '';
    final sec = DateTime.now().difference(t0).inMilliseconds / 1000.0;
    if (sec < 0.5) return '';
    final bps = _streamSampleCount * 2 / sec;
    return ' · ${(bps / 1024).toStringAsFixed(1)} KB/s';
  }

  Future<void> _startLiveStream() async {
    if (_busy || _device == null) {
      if (_device == null) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('请先扫描并连接 TestBLE')),
        );
      }
      return;
    }

    setState(() {
      _streamStarting = true;
      _waveSamples.clear();
      _decimBuffer.clear();
      _hpEma = null;
      _streamStartedAt = null;
      _streamRateSnapshotBps = null;
      _streamPacketCount = 0;
      _streamSampleCount = 0;
      _streamRateHz = BleProtocol.defaultStreamRateHz;
      _status = '正在连接并开启 Notify…';
    });
    _stopRssiMonitor();
    await _stopRssiScan();
    await _streamService.detachNotifyForTransfer(_log);
    BleTransferService.resetGattChain();
    await BleTransferService.drainGattChain();
    await Future<void>.delayed(
      Duration(milliseconds: Platform.isAndroid ? 500 : 250),
    );
    _log('--- 实时波形 (${BleProtocol.appRev}) ---');
    _log(
      '目标 ${(BleProtocol.streamTargetBytesPerSec / 1024).toStringAsFixed(1)} KB/s '
      '(${BleProtocol.defaultStreamRateHz} Hz × int16)',
    );

    try {
      final dev = await _ensureGattWarm(_device!);
      await _streamService.start(
        device: dev,
        onLog: _log,
        onPacket: (pkt) {
          if (!mounted) return;
          setState(() {
            _appendWaveSamples(pkt.samples);
            _streamPacketCount = _streamService.packetCount;
            _streamSampleCount = _streamService.totalSamples;
            if (_streamStartedAt == null) {
              _streamStartedAt = DateTime.now();
            }
            _status =
                '实时波形 · $_streamSampleCount 点 · $_streamPacketCount 包'
                '${_streamThroughputLabel()}'
                '${_streamLossLabel()}';
          });
        },
        onStopped: (total) async {
          if (!mounted) return;
          setState(() {
            _streaming = false;
            _streamStarting = false;
            _streamSampleCount = total;
            _freezeStreamRate();
            _status = '实时流已结束 · $total 点${_streamLossLabel()} · 可下载板子录制';
          });
        },
      );
      if (!mounted) return;
      setState(() {
        _streamStarting = false;
        _streaming = true;
        _streamStartedAt = DateTime.now();
        _status = '实时波形接收中…';
      });
    } catch (e) {
      _log('实时流错误: $e');
      _dropGattLink();
      if (mounted) {
        setState(() {
          _streaming = false;
          _streamStarting = false;
          _status = '实时流失败';
        });
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(
              '实时流失败: $e\n'
              '请：关开蓝牙 → 重新扫描连接 → 再试',
            ),
            duration: const Duration(seconds: 10),
          ),
        );
      }
    } finally {
      if (mounted && _device != null && !_streamService.isRunning) {
        if (!_streaming) {
          setState(() => _streamStarting = false);
        }
        _startRssiMonitor();
      }
    }
  }

  Future<void> _stopLiveStream() async {
    if (!_streaming) return;
    await _streamService.stop(onLog: _log, keepLink: true);
    final boardName = _streamService.lastRecordingBasename;
    if (mounted) {
      setState(() {
        _streaming = false;
        _freezeStreamRate();
        if (boardName != null && boardName.isNotEmpty) {
          _downloadNameCtrl.text = boardName;
          _status = '已停止 · 板子录制 $boardName · 点「下载最近录制」';
        } else {
          _status = '已停止 · 请点「下载最近录制」';
        }
      });
      _startRssiMonitor();
    }
  }

  void _freezeStreamRate() {
    final t0 = _streamStartedAt;
    if (t0 == null || _streamSampleCount <= 0) return;
    final sec = DateTime.now().difference(t0).inMilliseconds / 1000.0;
    if (sec >= 0.3) {
      _streamRateSnapshotBps = _streamSampleCount * 2 / sec;
    }
  }

  double _streamRateBps() {
    if (!_streaming) {
      return _streamRateSnapshotBps ?? 0;
    }
    final t0 = _streamStartedAt;
    if (t0 == null || _streamSampleCount <= 0) return 0;
    final sec = DateTime.now().difference(t0).inMilliseconds / 1000.0;
    if (sec < 0.3) return 0;
    return _streamSampleCount * 2 / sec;
  }

  Widget _metricChip(String label, String value, {Color? color}) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
      decoration: BoxDecoration(
        color: (color ?? Colors.teal).withValues(alpha: 0.08),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: (color ?? Colors.teal).withValues(alpha: 0.25)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        mainAxisSize: MainAxisSize.min,
        children: [
          Text(
            label,
            style: TextStyle(fontSize: 10, color: Colors.grey.shade600),
          ),
          const SizedBox(height: 2),
          Text(
            value,
            style: TextStyle(
              fontSize: 13,
              fontWeight: FontWeight.w600,
              color: color ?? Colors.teal.shade800,
            ),
          ),
        ],
      ),
    );
  }

  Widget _demoStep(int n, String label, bool active) {
    return Expanded(
      child: Row(
        children: [
          CircleAvatar(
            radius: 12,
            backgroundColor: active
                ? Theme.of(context).colorScheme.primary
                : Colors.grey.shade300,
            child: Text(
              '$n',
              style: TextStyle(
                fontSize: 12,
                fontWeight: FontWeight.bold,
                color: active ? Colors.white : Colors.grey.shade600,
              ),
            ),
          ),
          const SizedBox(width: 8),
          Expanded(
            child: Text(
              label,
              style: TextStyle(
                fontSize: 13,
                fontWeight: active ? FontWeight.w600 : FontWeight.normal,
                color: active ? Colors.black87 : Colors.grey.shade600,
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildConnectionCard() {
    final connected = _device != null;
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(
                  connected ? Icons.bluetooth_connected : Icons.bluetooth_disabled,
                  color: connected
                      ? Theme.of(context).colorScheme.primary
                      : Colors.grey,
                ),
                const SizedBox(width: 10),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(
                        connected ? BleProtocol.defaultName : '未连接板子',
                        style: Theme.of(context).textTheme.titleMedium?.copyWith(
                              fontWeight: FontWeight.w600,
                            ),
                      ),
                      Text(
                        connected
                            ? _device!.remoteId.str
                            : '请先扫描并连接',
                        style: Theme.of(context).textTheme.bodySmall,
                      ),
                    ],
                  ),
                ),
                FilledButton.tonal(
                  onPressed: _scanning || _busy ? null : _scan,
                  child: Text(_scanning ? '扫描中' : '扫描'),
                ),
                if (connected) ...[
                  const SizedBox(width: 4),
                  TextButton(
                    onPressed: _scanning || _busy
                        ? null
                        : () => unawaited(_disconnectFromUi()),
                    child: const Text('断开'),
                  ),
                  PopupMenuButton<String>(
                    tooltip: '更多连接选项',
                    enabled: !_scanning && !_busy,
                    onSelected: (value) {
                      if (value == 'forget') {
                        unawaited(_disconnectFromUi(forget: true));
                      }
                    },
                    itemBuilder: (ctx) => const [
                      PopupMenuItem(
                        value: 'forget',
                        child: Text('断开并忘记设备'),
                      ),
                    ],
                  ),
                ],
              ],
            ),
            if (connected && _rssi != null) ...[
              const SizedBox(height: 12),
              Row(
                children: [
                  Icon(Icons.signal_cellular_alt, color: _rssiColor(_rssi), size: 20),
                  const SizedBox(width: 6),
                  Text(
                    '${_rssi} dBm · ${_rssiLabel(_rssi!)}',
                    style: TextStyle(
                      fontWeight: FontWeight.w500,
                      color: _rssiColor(_rssi),
                    ),
                  ),
                ],
              ),
            ],
          ],
        ),
      ),
    );
  }

  Widget _buildLiveWaveformCard() {
    final cs = Theme.of(context).colorScheme;
    final rateBps = _streamRateBps();
    final lossS = _streamService.lostSamples;
    final lossP = _streamService.lostPackets;
    final lossOk = lossS == 0 && lossP == 0;

    return Card(
      color: cs.primaryContainer.withValues(alpha: 0.35),
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(16),
        side: BorderSide(color: cs.primary.withValues(alpha: 0.3)),
      ),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(Icons.monitor_heart_outlined, color: cs.primary),
                const SizedBox(width: 8),
                Expanded(
                  child: Text(
                    'fNIRS 实时波形',
                    style: Theme.of(context).textTheme.titleMedium?.copyWith(
                          fontWeight: FontWeight.w700,
                          color: cs.onPrimaryContainer,
                        ),
                  ),
                ),
                Container(
                  padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
                  decoration: BoxDecoration(
                    color: cs.surface.withValues(alpha: 0.7),
                    borderRadius: BorderRadius.circular(6),
                  ),
                  child: Text(
                    BleProtocol.appRev.split('-').last,
                    style: const TextStyle(fontSize: 10, fontFamily: 'monospace'),
                  ),
                ),
              ],
            ),
            const SizedBox(height: 12),
            Row(
              children: [
                _demoStep(1, '实时采集', _streaming || _streamSampleCount > 0),
                Icon(Icons.chevron_right, color: Colors.grey.shade400),
                _demoStep(2, '下载文件', _lastReceive != null),
              ],
            ),
            const SizedBox(height: 14),
            WaveformChart(
              samples: List<double>.from(_waveSamples),
              isLive: _streaming,
              caption: _waveSamples.isEmpty
                  ? '板子录制 record_*.bin · 停止后点「下载最近录制」'
                  : 'AC 成分 · ${_waveWindowSec.toStringAsFixed(0)}s 窗口 · $_waveDisplayHz Hz 显示',
            ),
            if (_streaming) ...[
              const SizedBox(height: 12),
              Wrap(
                spacing: 8,
                runSpacing: 8,
                children: [
                  _metricChip(
                    '流速率',
                    rateBps > 0
                        ? '${(rateBps / 1024).toStringAsFixed(1)} KB/s'
                        : '—',
                  ),
                  _metricChip('采样点', '$_streamSampleCount'),
                  _metricChip('Notify', '$_streamPacketCount 包'),
                  _metricChip(
                    '丢包',
                    lossOk ? '无' : '$lossS 点 / $lossP 包',
                    color: lossOk ? Colors.green : Colors.orange,
                  ),
                ],
              ),
            ] else if (_streamSampleCount > 0 &&
                _streamRateSnapshotBps != null) ...[
              const SizedBox(height: 12),
              Wrap(
                spacing: 8,
                runSpacing: 8,
                children: [
                  _metricChip(
                    '采集均速',
                    '${(_streamRateSnapshotBps! / 1024).toStringAsFixed(1)} KB/s',
                  ),
                  _metricChip('采样点', '$_streamSampleCount'),
                  _metricChip('Notify', '$_streamPacketCount 包'),
                  _metricChip(
                    '丢包',
                    lossOk ? '无' : '$lossS 点 / $lossP 包',
                    color: lossOk ? Colors.green : Colors.orange,
                  ),
                ],
              ),
            ],
            const SizedBox(height: 14),
            if (!_streaming && !_streamStarting && !_busy) ...[
              SizedBox(
                width: double.infinity,
                child: FilledButton.icon(
                  onPressed: _device == null ? null : _startLiveStream,
                  icon: const Icon(Icons.play_circle_fill),
                  label: const Text('开始实时波形'),
                ),
              ),
              const SizedBox(height: 8),
              SizedBox(
                width: double.infinity,
                child: OutlinedButton.icon(
                  onPressed: _device == null ? null : _downloadLastRecording,
                  icon: const Icon(Icons.cloud_download),
                  label: const Text('下载最近录制'),
                ),
              ),
            ] else if (_streamStarting) ...[
              const SizedBox(height: 8),
              const LinearProgressIndicator(),
              const SizedBox(height: 8),
              Text(
                _status,
                style: Theme.of(context).textTheme.bodySmall,
              ),
            ] else if (_streaming) ...[
              SizedBox(
                width: double.infinity,
                child: FilledButton.tonalIcon(
                  onPressed: _stopLiveStream,
                  style: FilledButton.styleFrom(
                    backgroundColor: Colors.red.shade50,
                    foregroundColor: Colors.red.shade800,
                  ),
                  icon: const Icon(Icons.stop_circle),
                  label: const Text('停止采集'),
                ),
              ),
              const SizedBox(height: 6),
              Text(
                _status,
                style: Theme.of(context).textTheme.bodySmall,
              ),
            ],
          ],
        ),
      ),
    );
  }

  Widget _buildDownloadCard() {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              '从板子下载',
              style: Theme.of(context).textTheme.titleMedium?.copyWith(
                    fontWeight: FontWeight.w600,
                  ),
            ),
            const SizedBox(height: 4),
            Text(
              '默认目录 ${BleProtocol.bleTxDir}/',
              style: Theme.of(context).textTheme.bodySmall,
            ),
            const SizedBox(height: 12),
            TextField(
              controller: _downloadNameCtrl,
              enabled: !_busy,
              decoration: InputDecoration(
                labelText: '文件名',
                hintText: 'record_YYYYMMDD_HHMMSS.bin',
                border: OutlineInputBorder(
                  borderRadius: BorderRadius.circular(12),
                ),
                isDense: true,
                prefixIcon: const Icon(Icons.description_outlined),
              ),
            ),
            if (_downloading) ...[
              const SizedBox(height: 12),
              LinearProgressIndicator(
                value: _progress.clamp(0.0, 1.0),
                borderRadius: BorderRadius.circular(4),
              ),
              const SizedBox(height: 8),
              Text(
                _status,
                style: Theme.of(context).textTheme.bodySmall?.copyWith(
                      fontFamily: 'monospace',
                      fontSize: 12,
                    ),
              ),
            ],
            if (_lastReceive != null) ...[
              const SizedBox(height: 12),
              _ReceiveSummaryCard(summary: _lastReceive!),
            ],
            const SizedBox(height: 12),
            if (!_busy)
              SizedBox(
                width: double.infinity,
                child: FilledButton.icon(
                  onPressed: _device == null ? null : _download,
                  icon: const Icon(Icons.download),
                  label: const Text('下载指定文件'),
                ),
              )
            else if (_downloading && _paused)
              Row(
                children: [
                  Expanded(
                    child: FilledButton.icon(
                      onPressed: _resumeTransfer,
                      icon: const Icon(Icons.play_arrow),
                      label: const Text('继续'),
                    ),
                  ),
                  const SizedBox(width: 8),
                  Expanded(
                    child: OutlinedButton.icon(
                      onPressed: _cancelTransfer,
                      icon: const Icon(Icons.close),
                      label: const Text('取消'),
                    ),
                  ),
                ],
              )
            else if (_downloading)
              FilledButton.tonalIcon(
                onPressed: _pauseTransfer,
                icon: const Icon(Icons.pause),
                label: const Text('暂停下载'),
              ),
          ],
        ),
      ),
    );
  }

  Widget _buildUploadSection() {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        ListTile(
          contentPadding: EdgeInsets.zero,
          title: const Text('文件', style: TextStyle(fontWeight: FontWeight.w600)),
          subtitle: Text(_filePath ?? '未选择'),
          trailing: FilledButton.tonal(
            onPressed: _busy ? null : _pickFile,
            child: const Text('选择'),
          ),
        ),
        SwitchListTile(
          contentPadding: EdgeInsets.zero,
          title: const Text('帧 CRC 校验'),
          subtitle: const Text('与 PC 端 --frame-crc 一致'),
          value: _frameCrc,
          onChanged: _busy ? null : (v) => setState(() => _frameCrc = v),
        ),
        SwitchListTile(
          contentPadding: EdgeInsets.zero,
          title: const Text('快速模式'),
          subtitle: const Text('MTU 247 + chunk 200 + Notify 流控'),
          value: _fastMode,
          onChanged: _busy ? null : (v) => setState(() => _fastMode = v),
        ),
        if (_sending) ...[
          const SizedBox(height: 8),
          LinearProgressIndicator(value: _progress.clamp(0.0, 1.0)),
        ],
        if (_lastSummary != null) ...[
          const SizedBox(height: 12),
          _SendSummaryCard(summary: _lastSummary!),
        ],
        const SizedBox(height: 12),
        if (!_busy)
          FilledButton.icon(
            onPressed: _device == null ? null : _send,
            icon: const Icon(Icons.upload_file),
            label: const Text('发送到板子'),
          )
        else if (_sending && _paused)
          Row(
            children: [
              Expanded(
                child: FilledButton.icon(
                  onPressed: _resumeTransfer,
                  icon: const Icon(Icons.play_arrow),
                  label: const Text('继续'),
                ),
              ),
              const SizedBox(width: 8),
              Expanded(
                child: OutlinedButton.icon(
                  onPressed: _cancelTransfer,
                  icon: const Icon(Icons.close),
                  label: const Text('取消'),
                ),
              ),
            ],
          )
        else if (_sending)
          FilledButton.tonalIcon(
            onPressed: _pauseTransfer,
            icon: const Icon(Icons.pause),
            label: const Text('暂停'),
          ),
      ],
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: IndexedStack(
        index: _tabIndex,
        children: [
          _buildDeviceTab(context),
          FnirsMonitorPage(
            key: ValueKey(_device?.remoteId.str ?? 'none'),
            device: _device,
          ),
        ],
      ),
      bottomNavigationBar: NavigationBar(
        selectedIndex: _tabIndex,
        onDestinationSelected: (i) {
          if (i == _tabIndex) return;
          if (i == 0) {
            setState(() => _tabIndex = i);
            _startRssiMonitor();
          } else {
            _stopRssiMonitor();
            setState(() => _tabIndex = i);
          }
        },
        destinations: const [
          NavigationDestination(
            icon: Icon(Icons.bluetooth_outlined),
            selectedIcon: Icon(Icons.bluetooth),
            label: '设备',
          ),
          NavigationDestination(
            icon: Icon(Icons.monitor_heart_outlined),
            selectedIcon: Icon(Icons.monitor_heart),
            label: '监测',
          ),
        ],
      ),
    );
  }

  Widget _buildDeviceTab(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text('fNIRS BLE', style: TextStyle(fontSize: 18)),
            Text(
              '连接 · 文件传输',
              style: TextStyle(fontSize: 12, fontWeight: FontWeight.normal),
            ),
          ],
        ),
        toolbarHeight: 56,
        backgroundColor: Theme.of(context).colorScheme.primaryContainer,
        foregroundColor: Theme.of(context).colorScheme.onPrimaryContainer,
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          _buildConnectionCard(),
          const SizedBox(height: 16),
          _buildDownloadCard(),
          const SizedBox(height: 8),
          ExpansionTile(
            tilePadding: EdgeInsets.zero,
            title: const Text(
              '高级：上传到板子',
              style: TextStyle(fontWeight: FontWeight.w600),
            ),
            subtitle: const Text('FILE RX · 手机 → 板子'),
            children: [
              Padding(
                padding: const EdgeInsets.only(bottom: 16),
                child: _buildUploadSection(),
              ),
            ],
          ),
          ExpansionTile(
            initiallyExpanded: false,
            tilePadding: EdgeInsets.zero,
            title: const Text(
              '调试日志',
              style: TextStyle(fontWeight: FontWeight.w600),
            ),
            children: [
              Container(
                height: 180,
                width: double.infinity,
                margin: const EdgeInsets.only(bottom: 16),
                padding: const EdgeInsets.all(12),
                decoration: BoxDecoration(
                  color: const Color(0xFF1E1E1E),
                  borderRadius: BorderRadius.circular(12),
                ),
                child: ListView.builder(
                  itemCount: _logs.length,
                  itemBuilder: (_, i) => Text(
                    _logs[i],
                    style: const TextStyle(
                      fontSize: 11,
                      fontFamily: 'monospace',
                      color: Color(0xFFB2DFDB),
                    ),
                  ),
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }
}

class _ReceiveSummary {
  const _ReceiveSummary({
    required this.filename,
    required this.fileSize,
    required this.fileCrc,
    required this.localPath,
    required this.displayPath,
    required this.publicVisible,
    required this.kbps,
    this.simInfo,
  });

  final String filename;
  final int fileSize;
  final int fileCrc;
  final String localPath;
  final String displayPath;
  final bool publicVisible;
  final double kbps;
  final SimSignalInfo? simInfo;

  String get crcHex => '0x${fileCrc.toRadixString(16).padLeft(8, '0')}';
}

class _ReceiveSummaryCard extends StatelessWidget {
  const _ReceiveSummaryCard({required this.summary});

  final _ReceiveSummary summary;

  @override
  Widget build(BuildContext context) {
    return Card(
      elevation: 0,
      color: Theme.of(context).colorScheme.primaryContainer.withValues(alpha: 0.4),
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(12),
        side: BorderSide(
          color: Theme.of(context).colorScheme.primary.withValues(alpha: 0.25),
        ),
      ),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(
                  Icons.download_done,
                  color: Theme.of(context).colorScheme.primary,
                  size: 28,
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: Text(
                    '下载完成 · ${_SendSummary.formatSize(summary.fileSize)}',
                    style: Theme.of(context).textTheme.titleMedium?.copyWith(
                          fontWeight: FontWeight.w600,
                        ),
                  ),
                ),
              ],
            ),
            const SizedBox(height: 12),
            if (summary.simInfo != null) ...[
              _row('信号', summary.simInfo!.demoSummary),
              _row('格式', 'fSIM v${summary.simInfo!.version}'),
            ],
            _row('文件名', summary.filename),
            _row('已接收', '${summary.fileSize} 字节 (100%)'),
            _row('速度', '${summary.kbps.toStringAsFixed(1)} KB/s'),
            _row('CRC32', '${summary.crcHex}（START 包核对）'),
            _row('保存位置', summary.displayPath),
            if (summary.localPath.contains('/'))
              _row('完整路径', summary.localPath),
            const Padding(
              padding: EdgeInsets.only(top: 6),
              child: Text(
                '请在文件管理器打开：内部存储 → 下载(Download) → TestBLE',
                style: TextStyle(fontSize: 12, color: Colors.black54),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _row(String label, String value) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 4),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          SizedBox(
            width: 64,
            child: Text(label, style: const TextStyle(color: Colors.black54, fontSize: 13)),
          ),
          Expanded(
            child: Text(value, style: const TextStyle(fontSize: 13, fontWeight: FontWeight.w500)),
          ),
        ],
      ),
    );
  }
}

class _SendSummary {
  const _SendSummary({
    required this.filename,
    required this.fileSize,
    required this.fileCrc,
    required this.kbps,
  });

  final String filename;
  final int fileSize;
  final int fileCrc;
  final double kbps;

  String get crcHex => '0x${fileCrc.toRadixString(16).padLeft(8, '0')}';

  static String formatSize(int bytes) {
    if (bytes < 1024) return '$bytes B';
    if (bytes < 1024 * 1024) return '${(bytes / 1024).toStringAsFixed(1)} KB';
    return '${(bytes / (1024 * 1024)).toStringAsFixed(2)} MB';
  }
}

class _SendSummaryCard extends StatelessWidget {
  const _SendSummaryCard({required this.summary});

  final _SendSummary summary;

  @override
  Widget build(BuildContext context) {
    return Card(
      elevation: 0,
      color: Colors.green.shade50,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(12),
        side: BorderSide(color: Colors.green.shade200),
      ),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(Icons.check_circle, color: Colors.green.shade700, size: 28),
                const SizedBox(width: 8),
                Expanded(
                  child: Text(
                    '发送完成 · ${_SendSummary.formatSize(summary.fileSize)}',
                    style: Theme.of(context).textTheme.titleMedium?.copyWith(
                          fontWeight: FontWeight.w600,
                          color: Colors.green.shade800,
                        ),
                  ),
                ),
              ],
            ),
            const SizedBox(height: 12),
            _row('文件名', summary.filename),
            _row('已发送', '${summary.fileSize} 字节 (100%)'),
            _row('速度', '${summary.kbps.toStringAsFixed(1)} KB/s'),
            _row('CRC32', '${summary.crcHex}（START 包，板子 END 后核对）'),
            _row('板子路径', '${BleProtocol.bleRxDir}/${summary.filename}'),
          ],
        ),
      ),
    );
  }

  Widget _row(String label, String value) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 4),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          SizedBox(
            width: 64,
            child: Text(label, style: const TextStyle(color: Colors.black54, fontSize: 13)),
          ),
          Expanded(
            child: Text(value, style: const TextStyle(fontSize: 13, fontWeight: FontWeight.w500)),
          ),
        ],
      ),
    );
  }
}
