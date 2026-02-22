# Xcode Project Setup Checklist

## 1. Create New Xcode Project

- [x] Open Xcode
- [x] File → New → Project
- [x] Choose "App" template
- [x] Product Name: `Firefly`
- [x] Interface: SwiftUI
- [x] Language: Swift
- [x] Minimum Deployment: iOS 17.0

## 2. Add SwiftProtobuf Package

- [ ] File → Add Package Dependencies
- [ ] Search for: `https://github.com/apple/swift-protobuf`
- [ ] Version: 1.25.0 or later
- [ ] Add to target: Firefly

## 3. Configure Info.plist

- [ ] Select Firefly target
- [ ] Go to "Info" tab
- [ ] Add custom iOS Target Properties:
  - [ ] Privacy - Bluetooth Always Usage Description
  - [ ] Privacy - Bluetooth Peripheral Usage Description
  - [ ] Privacy - Location When In Use Usage Description
- [ ] See `INFO_PLIST_GUIDE.md` for exact strings

## 4. Enable Background Modes (Optional)

- [ ] Select Firefly target
- [ ] Go to "Signing & Capabilities"
- [ ] Click "+ Capability"
- [ ] Add "Background Modes"
- [ ] Check:
  - [ ] Uses Bluetooth LE accessories
  - [ ] Remote notifications

## 5. Add Source Files

Copy all files from this implementation into your project:

### Core
- [ ] Core/Models/MeshNode.swift
- [ ] Core/Models/MeshMessage.swift
- [ ] Core/Models/MeshChannel.swift
- [ ] Core/Models/NodeLocation.swift
- [ ] Core/Protocols/BluetoothServiceProtocol.swift
- [ ] Core/Protocols/MeshtasticClientProtocol.swift
- [ ] Core/Protocols/MessagingServiceProtocol.swift
- [ ] Core/Protocols/MapServiceProtocol.swift
- [ ] Core/Protocols/NotificationServiceProtocol.swift
- [ ] Core/DependencyContainer.swift

### Services
- [ ] Services/Bluetooth/MeshtasticBLEConstants.swift
- [ ] Services/Bluetooth/CoreBluetoothService.swift
- [ ] Services/Meshtastic/MeshtasticClient.swift
- [ ] Services/Notification/LocalNotificationService.swift

### Domain
- [ ] Domain/Messaging/MessagingService.swift
- [ ] Domain/Map/MapService.swift

### ViewModels
- [ ] ViewModels/InboxViewModel.swift
- [ ] ViewModels/ConversationViewModel.swift
- [ ] ViewModels/MapViewModel.swift

### Views
- [ ] Views/InboxView.swift
- [ ] Views/ConversationView.swift
- [ ] Views/MapView.swift

### App
- [ ] Replace FireflyApp.swift with new implementation

## 6. Add Test Target

- [ ] File → New → Target
- [ ] Choose "Unit Testing Bundle"
- [ ] Target Name: FireflyTests
- [ ] Test Host: Firefly

### Add Test Files
- [ ] Tests/MessagingServiceTests.swift
- [ ] Tests/MeshtasticClientTests.swift
- [ ] Tests/InboxViewModelTests.swift
- [ ] Tests/ConversationViewModelTests.swift
- [ ] Tests/MapViewModelTests.swift
- [ ] Tests/DependencyContainerTests.swift

### Add Mocks to Test Target
- [ ] Mocks/MockBluetoothService.swift
- [ ] Mocks/MockMeshtasticClient.swift
- [ ] Mocks/MockMessagingService.swift
- [ ] Mocks/MockMapService.swift
- [ ] Mocks/MockNotificationService.swift

**Important**: Add mocks to BOTH main target AND test target!

## 7. Configure Test Target

- [ ] Select FireflyTests target
- [ ] Build Settings → Swift Compiler
- [ ] Enable Testing: Yes
- [ ] Add `@testable import Firefly` to test files

## 8. Project Organization

Organize files in Xcode:

```
Firefly/
├── App/
│   └── FireflyApp.swift
├── Core/
│   ├── Models/
│   ├── Protocols/
│   └── DependencyContainer.swift
├── Services/
│   ├── Bluetooth/
│   ├── Meshtastic/
│   └── Notification/
├── Domain/
│   ├── Messaging/
│   └── Map/
├── ViewModels/
└── Views/
```

## 9. Build & Test

- [ ] Clean build folder (Shift+Cmd+K)
- [ ] Build project (Cmd+B)
- [ ] Resolve any errors
- [ ] Run tests (Cmd+U)
- [ ] Verify all tests pass

## 10. Run the App

- [ ] Select iOS Simulator (iPhone 15 Pro recommended)
- [ ] Run app (Cmd+R)
- [ ] Verify UI appears correctly
- [ ] Check connection status bar
- [ ] Test navigation

## 11. Common Issues & Solutions

### Issue: "Cannot find type 'Data_pb' in scope"
**Solution**: Make sure all protobuf files are in the project and SwiftProtobuf package is linked

### Issue: "Module 'Firefly' has no member 'MeshNode'"
**Solution**: Ensure files are added to correct target (Firefly, not just project)

### Issue: Tests don't compile
**Solution**: Make sure mocks are added to test target and use `@testable import Firefly`

### Issue: Bluetooth permissions alert doesn't appear
**Solution**: Add Info.plist entries, delete app from simulator, and re-run

### Issue: Map doesn't show
**Solution**: Add location permissions to Info.plist

## 12. Verify Installation

Run this checklist:

- [ ] App builds without errors
- [ ] All 29 tests pass
- [ ] InboxView shows with dark theme
- [ ] Connection status bar visible
- [ ] Map sheet opens correctly
- [ ] No console errors on launch

## 13. Optional Enhancements

- [ ] Add app icon
- [ ] Configure launch screen
- [ ] Add localization
- [ ] Configure code signing
- [ ] Add CI/CD pipeline
- [ ] Set up Analytics

## 14. Documentation

- [ ] Read Architecture.md
- [ ] Review IMPLEMENTATION_GUIDE.md
- [ ] Check PROJECT_SUMMARY.md

## Ready to Code! 🚀

Once all checkboxes are complete, you're ready to:
- Connect to a real Meshtastic device
- Implement full protobuf support
- Add persistence layer
- Build additional features

For questions, refer to:
- `IMPLEMENTATION_GUIDE.md` - How to extend the codebase
- `Architecture.md` - System design overview
- `README.md` - Feature documentation
