import 'package:flutter/cupertino.dart';
import 'package:sembast/sembast.dart';
import 'package:sembast/sembast_io.dart';
import 'package:path_provider/path_provider.dart';

import 'main.dart';

class UrlStores {
  static final UrlStores instance = UrlStores();

  Database? _db;

  Map<String, PlayType> _urls = {};

  Future<Database> _openDb() async {
    if (_db == null) {
      final dir = await getApplicationSupportDirectory();
      final path = "${dir.absolute.path}/example.db";
      debugPrint("path = ${path}");
      _db = await databaseFactoryIo.openDatabase(path);
    }
    return _db!;
  }

  Future<Map<String, PlayType>> getUrls() async {
    if (_urls.isNotEmpty) {
      return Map.from(_urls);
    }
    final db = await _openDb();
    final urls = await StoreRef<String, Map<String, Object?>>.main()
        .record("data")
        .get(db);
    if (urls != null) {
      _urls.addAll(urls.map((key, value) =>
          MapEntry(key, PlayType.values[int.parse(value.toString())])));
    }
    return Map.from(_urls);
  }

  Future<void> put(String url, PlayType type) async {
    _urls[url] = type;
    final db = await _openDb();
    await StoreRef<String, Map<String, Object?>>.main()
        .record("data")
        .put(db, _urls.map((key, value) => MapEntry(key, value.index)));
  }

  Future<void> remove(String url) async {
    _urls.remove(url);
    final db = await _openDb();
    await StoreRef<String, Map<String, Object?>>.main()
        .record("data")
        .put(db, _urls.map((key, value) => MapEntry(key, value.index)));
  }
}
