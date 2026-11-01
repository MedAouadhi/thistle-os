import 'dart:convert';
import 'dart:io';

/// The protocol's test vectors, docs/tailnet-chat/vectors/, seen from this
/// package (tests run from companion/core).
final Uri vectorsDir = Directory.current.uri.resolve('../../docs/tailnet-chat/vectors/');

Map<String, dynamic> vector(String name) =>
    jsonDecode(File.fromUri(vectorsDir.resolve(name)).readAsStringSync()) as Map<String, dynamic>;
