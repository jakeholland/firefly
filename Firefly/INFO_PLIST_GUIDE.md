# Required Info.plist Entries

Add these entries to your Info.plist file for Firefly to work properly:

## Bluetooth Permissions

```xml
<key>NSBluetoothAlwaysUsageDescription</key>
<string>Firefly needs Bluetooth to connect to your Meshtastic device for mesh messaging.</string>

<key>NSBluetoothPeripheralUsageDescription</key>
<string>Firefly uses Bluetooth to communicate with your Meshtastic device.</string>
```

## Location Permissions (for Map)

```xml
<key>NSLocationWhenInUseUsageDescription</key>
<string>Firefly needs your location to show you on the map with your friends.</string>

<key>NSLocationAlwaysUsageDescription</key>
<string>Firefly uses your location to keep your position updated on the mesh map.</string>
```

## Background Modes (Optional)

If you want Bluetooth to work in background:

```xml
<key>UIBackgroundModes</key>
<array>
    <string>bluetooth-central</string>
    <string>remote-notification</string>
</array>
```

## Complete Info.plist Example

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <!-- Bluetooth Permissions -->
    <key>NSBluetoothAlwaysUsageDescription</key>
    <string>Firefly needs Bluetooth to connect to your Meshtastic device for mesh messaging.</string>
    
    <key>NSBluetoothPeripheralUsageDescription</key>
    <string>Firefly uses Bluetooth to communicate with your Meshtastic device.</string>
    
    <!-- Location Permissions -->
    <key>NSLocationWhenInUseUsageDescription</key>
    <string>Firefly needs your location to show you on the map with your friends.</string>
    
    <key>NSLocationAlwaysUsageDescription</key>
    <string>Firefly uses your location to keep your position updated on the mesh map.</string>
    
    <!-- Background Modes -->
    <key>UIBackgroundModes</key>
    <array>
        <string>bluetooth-central</string>
        <string>remote-notification</string>
    </array>
    
    <!-- App Configuration -->
    <key>CFBundleDevelopmentRegion</key>
    <string>$(DEVELOPMENT_LANGUAGE)</string>
    
    <key>CFBundleExecutable</key>
    <string>$(EXECUTABLE_NAME)</string>
    
    <key>CFBundleIdentifier</key>
    <string>$(PRODUCT_BUNDLE_IDENTIFIER)</string>
    
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    
    <key>CFBundleName</key>
    <string>$(PRODUCT_NAME)</string>
    
    <key>CFBundlePackageType</key>
    <string>$(PRODUCT_BUNDLE_PACKAGE_TYPE)</string>
    
    <key>CFBundleShortVersionString</key>
    <string>1.0</string>
    
    <key>CFBundleVersion</key>
    <string>1</string>
    
    <key>LSRequiresIPhoneOS</key>
    <true/>
    
    <key>UIApplicationSceneManifest</key>
    <dict>
        <key>UIApplicationSupportsMultipleScenes</key>
        <true/>
    </dict>
    
    <key>UILaunchScreen</key>
    <dict/>
    
    <key>UIRequiredDeviceCapabilities</key>
    <array>
        <string>armv7</string>
    </array>
    
    <key>UISupportedInterfaceOrientations</key>
    <array>
        <string>UIInterfaceOrientationPortrait</string>
        <string>UIInterfaceOrientationLandscapeLeft</string>
        <string>UIInterfaceOrientationLandscapeRight</string>
    </array>
</dict>
</plist>
```

## Notes

1. **Bluetooth permissions are required** - The app won't be able to scan or connect without them
2. **Location permissions are optional** - Only needed if you want to show user's location on map
3. **Background modes are optional** - Only enable if you need background Bluetooth connectivity
4. **Customize the strings** - Make them user-friendly and explain why you need each permission
