import 'package:flutter_test/flutter_test.dart';
import 'package:thistle_chat/src/permissions.dart';

import 'helpers.dart';

void main() {
  late FakePermissions platform;
  late Permissions permissions;

  setUp(() {
    platform = FakePermissions();
    permissions = Permissions(platform);
  });

  test('asked when first needed, and not again once granted', () async {
    expect(await permissions.need(AppPermission.microphone), isTrue);
    expect(await permissions.need(AppPermission.microphone), isTrue);
    expect(platform.asked, [AppPermission.microphone]);
    expect(permissions.refusal(AppPermission.microphone), isNull);
  });

  test('already granted (Android before 13 for notifications): never asked', () async {
    platform.state[AppPermission.notifications] = PermissionAnswer.granted;
    expect(await permissions.need(AppPermission.notifications), isTrue);
    expect(platform.asked, isEmpty);
  });

  test('refused: the refusal says what does not work', () async {
    platform.answer = PermissionAnswer.denied;
    expect(await permissions.need(AppPermission.microphone), isFalse);
    expect(
      permissions.refusal(AppPermission.microphone),
      'The microphone is not allowed, so voice messages and calls cannot be recorded. '
      'Allow it when asked again, or in Android\'s settings for Thistle Chat.',
    );
    expect(permissions.refusals.keys, [AppPermission.microphone]);
  });

  test('refused for good: not asked again, and the explanation points to the settings', () async {
    platform.answer = PermissionAnswer.permanentlyDenied;
    expect(await permissions.need(AppPermission.notifications), isFalse);
    expect(await permissions.need(AppPermission.notifications), isFalse);
    expect(platform.asked, [AppPermission.notifications]);
    expect(permissions.refusal(AppPermission.notifications), contains('Notifications are not allowed'));
    expect(permissions.refusal(AppPermission.notifications), contains('Android\'s settings'));
    expect(permissions.forGood(AppPermission.notifications), isTrue);
    await permissions.openSettings();
    expect(platform.settingsOpened, 1);
  });

  test('allowed later: the refusal goes', () async {
    platform.answer = PermissionAnswer.denied;
    await permissions.need(AppPermission.microphone);
    platform.answer = PermissionAnswer.granted;
    expect(await permissions.need(AppPermission.microphone), isTrue);
    expect(permissions.refusal(AppPermission.microphone), isNull);
  });

  test('dismissed: the explanation is hidden until the next refusal', () async {
    platform.answer = PermissionAnswer.denied;
    await permissions.need(AppPermission.microphone);
    permissions.dismiss(AppPermission.microphone);
    expect(permissions.refusal(AppPermission.microphone), isNull);
    await permissions.need(AppPermission.microphone);
    expect(permissions.refusal(AppPermission.microphone), isNotNull);
  });
}
