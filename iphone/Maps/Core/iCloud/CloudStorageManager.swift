enum VoidResult {
  case success
  case failure(Error)
}
// TODO: Remove this type and use custom UTTypeIdentifier that is registered into the Info.plist after updating to the iOS >= 14.0.
struct FileType {
  let fileExtension: String
  let typeIdentifier: String
}

extension FileType {
  static let kml = FileType(fileExtension: "kml", typeIdentifier: "com.google.earth.kml")
}

typealias VoidResultCompletionHandler = (VoidResult) -> Void

let kTrashDirectoryName = ".Trash"
private let kBookmarksDirectoryName = "bookmarks"
private let kICloudSynchronizationDidChangeEnabledStateNotificationName = "iCloudSynchronizationDidChangeEnabledStateNotification"
private let kUDDidFinishInitialCloudSynchronization = "kUDDidFinishInitialCloudSynchronization"

@objc @objcMembers final class CloudStorageManager: NSObject {

  fileprivate struct Observation {
    weak var observer: AnyObject?
    var onErrorCompletionHandler: ((NSError?) -> Void)?
    var onSynchronizationIsInProcessCompletionHandler: ((Bool) -> Void)?
  }

  let fileManager: FileManager
  private let localDirectoryMonitor: LocalDirectoryMonitor
  private let cloudDirectoryMonitor: CloudDirectoryMonitor
  private let settings: Settings.Type
  private let bookmarksManager: BookmarksManager
  private let synchronizationStateManager: SynchronizationStateManager

  private let fileCoordinator = NSFileCoordinator()
  private let backgroundQueue = DispatchQueue(label: "iCloud.app.organicmaps.backgroundQueue", qos: .background)
  private var backgroundTaskIdentifier: UIBackgroundTaskIdentifier = .invalid
  private var localDirectoryUrl: URL { localDirectoryMonitor.directory }
  private var needsToReloadBookmarksOnTheMap = false
  private var semaphore: DispatchSemaphore?
  private var observers = [ObjectIdentifier: CloudStorageManager.Observation]()
  private var synchronizationIsInProcess = false {
    didSet { notifyObserversOnSynchronizationIsInProcess(synchronizationIsInProcess) }
  }
  private var synchronizationError: SynchronizationError? {
    didSet { notifyObserversOnSynchronizationError(synchronizationError) }
  }

  static private var isInitialSynchronization: Bool {
    return !UserDefaults.standard.bool(forKey: kUDDidFinishInitialCloudSynchronization)
  }

  static let shared: CloudStorageManager = {
    let fileManager = FileManager.default
    let fileType = FileType.kml
    let cloudDirectoryMonitor = iCloudDocumentsDirectoryMonitor(fileManager: fileManager, fileType: fileType)
    let synchronizationStateManager = DefaultSynchronizationStateManager(isInitialSynchronization: CloudStorageManager.isInitialSynchronization)
    do {
      let localDirectoryMonitor = try DefaultLocalDirectoryMonitor(fileManager: fileManager, directory: fileManager.bookmarksDirectoryUrl, fileType: fileType)
      let clodStorageManager = try CloudStorageManager(fileManager: fileManager,
                                                       settings: Settings.self,
                                                       bookmarksManager: BookmarksManager.shared(),
                                                       cloudDirectoryMonitor: cloudDirectoryMonitor,
                                                       localDirectoryMonitor: localDirectoryMonitor,
                                                       synchronizationStateManager: synchronizationStateManager)
      return clodStorageManager
    } catch {
      fatalError("Failed to create shared iCloud storage manager with error: \(error)")
    }
  }()

  // MARK: - Initialization
  init(fileManager: FileManager,
       settings: Settings.Type,
       bookmarksManager: BookmarksManager,
       cloudDirectoryMonitor: CloudDirectoryMonitor,
       localDirectoryMonitor: LocalDirectoryMonitor,
       synchronizationStateManager: SynchronizationStateManager) throws {
    guard fileManager === cloudDirectoryMonitor.fileManager, fileManager === localDirectoryMonitor.fileManager else {
      throw NSError(domain: "CloudStorageManger", code: 0, userInfo: [NSLocalizedDescriptionKey: "File managers should be the same."])
    }
    self.fileManager = fileManager
    self.settings = settings
    self.bookmarksManager = bookmarksManager
    self.cloudDirectoryMonitor = cloudDirectoryMonitor
    self.localDirectoryMonitor = localDirectoryMonitor
    self.synchronizationStateManager = synchronizationStateManager
    super.init()
  }

  // MARK: - Public
  @objc func start() {
    subscribeToSettingsNotifications()
    subscribeToApplicationLifecycleNotifications()
    cloudDirectoryMonitor.delegate = self
    localDirectoryMonitor.delegate = self
  }
}

// MARK: - Private
private extension CloudStorageManager {
  // MARK: - Synchronization Lifecycle
  func startSynchronization() {
    LOG(.debug, "Start synchronization...")
    switch cloudDirectoryMonitor.state {
    case .started:
      LOG(.debug, "Synchronization is already started")
      return
    case .paused:
      resumeSynchronization()
    case .stopped:
      cloudDirectoryMonitor.start { [weak self] result in
        guard let self else { return }
        switch result {
        case .failure(let error):
          self.stopSynchronization()
          self.processError(error)
        case .success:
          self.localDirectoryMonitor.start { result in
            switch result {
            case .failure(let error):
              self.stopSynchronization()
              self.processError(error)
            case .success:
              LOG(.debug, "Synchronization is started successfully")
              self.addToBookmarksManagerObserverList()
              break
            }
          }
        }
      }
    }
  }

  func stopSynchronization() {
    LOG(.debug, "Stop synchronization")
    localDirectoryMonitor.stop()
    cloudDirectoryMonitor.stop()
    synchronizationError = nil
    synchronizationStateManager.resetState()
    removeFromBookmarksManagerObserverList()
  }

  func pauseSynchronization() {
    LOG(.debug, "Pause synchronization")
    localDirectoryMonitor.pause()
    cloudDirectoryMonitor.pause()
  }

  func resumeSynchronization() {
    LOG(.debug, "Resume synchronization")
    localDirectoryMonitor.resume()
    cloudDirectoryMonitor.resume()
  }

  // MARK: - App Lifecycle
  func subscribeToApplicationLifecycleNotifications() {
    NotificationCenter.default.addObserver(self, selector: #selector(appWillEnterForeground), name: UIApplication.didBecomeActiveNotification, object: nil)
    NotificationCenter.default.addObserver(self, selector: #selector(appDidEnterBackground), name: UIApplication.didEnterBackgroundNotification, object: nil)
  }

  func unsubscribeFromApplicationLifecycleNotifications() {
    NotificationCenter.default.removeObserver(self, name: UIApplication.didBecomeActiveNotification, object: nil)
    NotificationCenter.default.removeObserver(self, name: UIApplication.didEnterBackgroundNotification, object: nil)
  }

  func subscribeToSettingsNotifications() {
    NotificationCenter.default.addObserver(self, selector: #selector(didChangeEnabledState), name: NSNotification.iCloudSynchronizationDidChangeEnabledState, object: nil)
  }

  @objc func appWillEnterForeground() {
    cancelBackgroundExecutionIfNeeded()
    guard settings.iCLoudSynchronizationEnabled() else { return }
    startSynchronization()
  }

  @objc func appDidEnterBackground() {
    guard settings.iCLoudSynchronizationEnabled() else { return }
    extendBackgroundExecutionIfNeeded { [weak self] in
      guard let self else { return }
      self.pauseSynchronization()
      self.cancelBackgroundExecutionIfNeeded()
    }
  }

  @objc func didChangeEnabledState() {
    settings.iCLoudSynchronizationEnabled() ? startSynchronization() : stopSynchronization()
  }

  // MARK: - BookmarksManager observing
  func addToBookmarksManagerObserverList() {
    bookmarksManager.add(self)
  }

  func removeFromBookmarksManagerObserverList() {
    bookmarksManager.remove(self)
  }

  func areBookmarksManagerNotificationsEnabled() -> Bool {
    bookmarksManager.areNotificationsEnabled()
  }
}

// MARK: - iCloudStorageManger + LocalDirectoryMonitorDelegate
extension CloudStorageManager: LocalDirectoryMonitorDelegate {
  func didFinishGathering(contents: LocalContents) {
    let events = synchronizationStateManager.resolveEvent(.didFinishGatheringLocalContents(contents))
    processEvents(events)
  }

  func didUpdate(contents: LocalContents) {
    let events = synchronizationStateManager.resolveEvent(.didUpdateLocalContents(contents))
    processEvents(events)
  }

  func didReceiveLocalMonitorError(_ error: Error) {
    processError(error)
  }
}

// MARK: - iCloudStorageManger + CloudDirectoryMonitorDelegate
extension CloudStorageManager: CloudDirectoryMonitorDelegate {
  func didFinishGathering(contents: CloudContents) {
    let events = synchronizationStateManager.resolveEvent(.didFinishGatheringCloudContents(contents))
    processEvents(events)
  }

  func didUpdate(contents: CloudContents) {
    let events = synchronizationStateManager.resolveEvent(.didUpdateCloudContents(contents))
    processEvents(events)
  }

  func didReceiveCloudMonitorError(_ error: Error) {
    processError(error)
  }
}

private extension CloudStorageManager {
  // MARK: - Handle Events
  func processEvents(_ events: [OutgoingEvent]) {
    guard !events.isEmpty else {
      synchronizationError = nil
      return
    }

    synchronizationIsInProcess = true

    LOG(.debug, "Processing events...")
    events.forEach { [weak self] event in
      guard let self else { return }
      self.backgroundQueue.async {
        LOG(.debug, "Execute event: \(event)")
        self.executeEvent(event)
      }
    }

    backgroundQueue.async { [self] in
      synchronizationIsInProcess = false
      reloadBookmarksOnTheMapIfNeeded()
      cancelBackgroundExecutionIfNeeded()
    }
  }

  func executeEvent(_ event: OutgoingEvent) {
    switch event {
    case .createLocalItem(let cloudMetadataItem): writeToLocalContainer(cloudMetadataItem, completion: completionHandler)
    case .updateLocalItem(let cloudMetadataItem): writeToLocalContainer(cloudMetadataItem, completion: completionHandler)
    case .removeLocalItem(let cloudMetadataItem): removeFromTheLocalContainer(cloudMetadataItem, completion: completionHandler)
    case .startDownloading(let cloudMetadataItem): startDownloading(cloudMetadataItem, completion: completionHandler)
    case .createCloudItem(let localMetadataItem): writeToCloudContainer(localMetadataItem, completion: completionHandler)
    case .updateCloudItem(let localMetadataItem): writeToCloudContainer(localMetadataItem, completion: completionHandler)
    case .removeCloudItem(let localMetadataItem): removeFromCloudContainer(localMetadataItem, completion: completionHandler)
    case .resolveVersionsConflict(let cloudMetadataItem): resolveVersionsConflict(cloudMetadataItem, completion: completionHandler)
    case .resolveInitialSynchronizationConflict(let localMetadataItem): resolveInitialSynchronizationConflict(localMetadataItem, completion: completionHandler)
    case .didFinishInitialSynchronization: UserDefaults.standard.set(true, forKey: kUDDidFinishInitialCloudSynchronization)
    case .didReceiveError(let error): processError(error)
    }
  }

  func completionHandler(result: VoidResult) {
    switch result {
    case .failure(let error):
      processError(error)
    case .success:
      break
    }
  }

  func reloadBookmarksOnTheMapIfNeeded() {
    guard needsToReloadBookmarksOnTheMap else { return }
    LOG(.debug, "Start reloading bookmarks on the map...")
    needsToReloadBookmarksOnTheMap = false
    semaphore = DispatchSemaphore(value: 0)
    DispatchQueue.main.async {
      // TODO: implement method in the c++ bookmarks manager to reload only updated category
      self.bookmarksManager.loadBookmarks()
    }
    semaphore?.wait()
    semaphore = nil
    LOG(.debug, "Bookmarks on the map are reloaded. Semaphore is released.")
  }

  // MARK: - Read/Write/Downloading/Uploading
  func startDownloading(_ cloudMetadataItem: CloudMetadataItem, completion: VoidResultCompletionHandler) {
    do {
      LOG(.debug, "Start downloading file: \(cloudMetadataItem.fileName)...")
      try fileManager.startDownloadingUbiquitousItem(at: cloudMetadataItem.fileUrl)
      completion(.success)
    } catch {
      completion(.failure(error))
    }
  }

  func writeToLocalContainer(_ cloudMetadataItem: CloudMetadataItem, completion: VoidResultCompletionHandler) {
    var coordinationError: NSError?
    let targetLocalFileUrl = cloudMetadataItem.relatedLocalItemUrl(to: localDirectoryUrl)
    LOG(.debug, "File \(cloudMetadataItem.fileName) is downloaded to the local iCloud container. Start coordinating and writing file...")
    fileCoordinator.coordinate(readingItemAt: cloudMetadataItem.fileUrl, writingItemAt: targetLocalFileUrl, error: &coordinationError) { readingUrl, writingUrl in
      do {
        let cloudFileData = try Data(contentsOf: readingUrl)
        try cloudFileData.write(to: writingUrl, options: .atomic, lastModificationDate: cloudMetadataItem.lastModificationDate)
        needsToReloadBookmarksOnTheMap = true
        LOG(.debug, "File \(cloudMetadataItem.fileName) is copied to local directory successfully.")
        completion(.success)
      } catch {
        completion(.failure(error))
      }
      return
    }
    if let coordinationError {
      completion(.failure(coordinationError))
    }
  }

  func removeFromTheLocalContainer(_ cloudMetadataItem: CloudMetadataItem, completion: VoidResultCompletionHandler) {
    LOG(.debug, "Start removing file \(cloudMetadataItem.fileName) from the local directory...")
    let targetLocalFileUrl = cloudMetadataItem.relatedLocalItemUrl(to: localDirectoryUrl)

    guard fileManager.fileExists(atPath: targetLocalFileUrl.path) else {
      LOG(.debug, "File \(cloudMetadataItem.fileName) doesn't exist in the local directory and cannot be removed.")
      completion(.success)
      return
    }

    do {
      try fileManager.removeItem(at: targetLocalFileUrl)
      needsToReloadBookmarksOnTheMap = true
      LOG(.debug, "File \(cloudMetadataItem.fileName) was removed from the local directory successfully.")
      completion(.success)
    } catch {
      completion(.failure(error))
    }
  }

  func writeToCloudContainer(_ localMetadataItem: LocalMetadataItem, completion: @escaping VoidResultCompletionHandler) {
    LOG(.debug, "Start writing file \(localMetadataItem.fileName) to the cloud directory...")
    cloudDirectoryMonitor.fetchUbiquityDirectoryUrl { [weak self] result in
      guard let self else { return }
      switch result {
      case .failure(let error):
        completion(.failure(error))
      case .success(let cloudDirectoryUrl):
        let targetCloudFileUrl = localMetadataItem.relatedCloudItemUrl(to: cloudDirectoryUrl)
        var coordinationError: NSError?
        fileCoordinator.coordinate(readingItemAt: localMetadataItem.fileUrl, writingItemAt: targetCloudFileUrl, error: &coordinationError) { readingUrl, writingUrl in
          do {
            let fileData = try localMetadataItem.fileData()
            try fileData.write(to: writingUrl, lastModificationDate: localMetadataItem.lastModificationDate)
            LOG(.debug, "File \(localMetadataItem.fileName) is copied to the cloud directory successfully.")
            completion(.success)
          } catch {
            completion(.failure(error))
          }
          return
        }
        if let coordinationError {
          completion(.failure(coordinationError))
        }
      }
    }
  }

  func removeFromCloudContainer(_ localMetadataItem: LocalMetadataItem, completion: @escaping VoidResultCompletionHandler) {
    LOG(.debug, "Start trashing file \(localMetadataItem.fileName)...")
    cloudDirectoryMonitor.fetchUbiquityDirectoryUrl { [weak self] result in
      guard let self else { return }
      switch result {
      case .failure(let error):
        completion(.failure(error))
      case .success(let cloudDirectoryUrl):
        do {
          let targetCloudFileUrl = localMetadataItem.relatedCloudItemUrl(to: cloudDirectoryUrl)
          try removeDuplicatedFileFromTrashDirectoryIfNeeded(cloudDirectoryUrl: cloudDirectoryUrl, fileName: localMetadataItem.fileName)
          try self.fileManager.trashItem(at: targetCloudFileUrl, resultingItemURL: nil)
          LOG(.debug, "File \(localMetadataItem.fileName) was trashed successfully.")
          completion(.success)
        } catch {
          completion(.failure(error))
        }
      }
    }

    // Remove duplicated file from iCloud's .Trash directory if needed.
    // It's important to avoid the duplicating of names in the trash because we can't control the name of the trashed item.
    func removeDuplicatedFileFromTrashDirectoryIfNeeded(cloudDirectoryUrl: URL, fileName: String) throws {
      // There are no ways to retrieve the content of iCloud's .Trash directory on macOS.
      if #available(iOS 14.0, *), ProcessInfo.processInfo.isiOSAppOnMac {
        return
      }
      LOG(.debug, "Checking if the file \(fileName) is already in the trash directory...")
      let trashDirectoryUrl = try fileManager.trashDirectoryUrl(for: cloudDirectoryUrl)
      let fileInTrashDirectoryUrl = trashDirectoryUrl.appendingPathComponent(fileName)
      let trashDirectoryContent = try fileManager.contentsOfDirectory(at: trashDirectoryUrl,
                                                                              includingPropertiesForKeys: [],
                                                                              options: [.skipsPackageDescendants, .skipsSubdirectoryDescendants])
      if trashDirectoryContent.contains(fileInTrashDirectoryUrl) {
        LOG(.debug, "File \(fileName) is already in the trash directory. Removing it...")
        try fileManager.removeItem(at: fileInTrashDirectoryUrl)
        LOG(.debug, "File \(fileName) was removed from the trash directory successfully.")
      }
    }
  }

  // MARK: - Error handling
  func processError(_ error: Error) {
    if let synchronizationError = error as? SynchronizationError {
      LOG(.debug, "Synchronization error: \(error.localizedDescription)")
      switch synchronizationError {
      case .fileUnavailable, .fileNotUploadedDueToQuota, .ubiquityServerNotAvailable:
        break
      case .iCloudIsNotAvailable, .containerNotFound:
        stopSynchronization()
      case .failedToOpenLocalDirectoryFileDescriptor, .failedToRetrieveLocalDirectoryContent:
        stopSynchronization()
      }
      self.synchronizationError = synchronizationError
    } else {
      // TODO: Handle non-synchronization errors
      LOG(.debug, "Non-synchronization error: \(error.localizedDescription)")
    }
  }

  // MARK: - Merge conflicts resolving
  func resolveVersionsConflict(_ cloudMetadataItem: CloudMetadataItem, completion: VoidResultCompletionHandler) {
    LOG(.debug, "Start resolving version conflict for file \(cloudMetadataItem.fileName)...")

    guard let versionsInConflict = NSFileVersion.unresolvedConflictVersionsOfItem(at: cloudMetadataItem.fileUrl),
          let currentVersion = NSFileVersion.currentVersionOfItem(at: cloudMetadataItem.fileUrl) else {
      LOG(.debug, "No versions in conflict found for file \(cloudMetadataItem.fileName).")
      completion(.success)
      return
    }

    let sortedVersions = versionsInConflict.sorted { version1, version2 in
      guard let date1 = version1.modificationDate, let date2 = version2.modificationDate else {
        return false
      }
      return date1 > date2
    }

    guard let latestVersionInConflict = sortedVersions.first else {
      LOG(.debug, "No latest version in conflict found for file \(cloudMetadataItem.fileName).")
      completion(.success)
      return
    }

    let targetCloudFileCopyUrl = generateNewFileUrl(for: cloudMetadataItem.fileUrl)
    var coordinationError: NSError?
    fileCoordinator.coordinate(writingItemAt: currentVersion.url,
                               options: [],
                               writingItemAt: targetCloudFileCopyUrl,
                               options: .forReplacing,
                               error: &coordinationError) { readingURL, writingURL in
      guard !fileManager.fileExists(atPath: targetCloudFileCopyUrl.path) else {
        needsToReloadBookmarksOnTheMap = true
        LOG(.debug, "File \(cloudMetadataItem.fileName) was already resolved.")
        completion(.success)
        return
      }
      do {
        LOG(.debug, "Copying file \(cloudMetadataItem.fileName)...")
        try fileManager.copyItem(at: readingURL, to: writingURL)
        try latestVersionInConflict.replaceItem(at: readingURL)
        try NSFileVersion.removeOtherVersionsOfItem(at: readingURL)
        needsToReloadBookmarksOnTheMap = true
        LOG(.debug, "File \(cloudMetadataItem.fileName) was successfully resolved.")
        completion(.success)
      } catch {
        completion(.failure(error))
      }
      return
    }

    if let coordinationError {
      completion(.failure(coordinationError))
    }
  }

  func resolveInitialSynchronizationConflict(_ localMetadataItem: LocalMetadataItem, completion: VoidResultCompletionHandler) {
    LOG(.debug, "Start resolving initial sync conflict for file \(localMetadataItem.fileName) by copying with a new name...")
    do {
      try fileManager.copyItem(at: localMetadataItem.fileUrl, to: generateNewFileUrl(for: localMetadataItem.fileUrl, addDeviceName: true))
      LOG(.debug, "File \(localMetadataItem.fileName) was successfully resolved.")
      completion(.success)
    } catch {
      completion(.failure(error))
    }
    return
  }

  // MARK: - Helper methods
  func generateNewFileUrl(for fileUrl: URL, addDeviceName: Bool = false) -> URL {
    let baseName = fileUrl.deletingPathExtension().lastPathComponent
    let fileExtension = fileUrl.pathExtension

    let regexPattern = "_(\\d+)$"
    let regex = try! NSRegularExpression(pattern: regexPattern)
    let range = NSRange(location: 0, length: baseName.utf16.count)
    let matches = regex.matches(in: baseName, options: [], range: range)

    var finalBaseName = baseName

    if let match = matches.last, let existingNumberRange = Range(match.range(at: 1), in: baseName) {
      let existingNumber = Int(baseName[existingNumberRange])!
      let incrementedNumber = existingNumber + 1
      finalBaseName = baseName.replacingCharacters(in: existingNumberRange, with: "\(incrementedNumber)")
    } else {
      finalBaseName = baseName + "_1"
    }
    let deviceName = addDeviceName ? "_\(UIDevice.current.name)" : ""
    let newFileName = finalBaseName + deviceName + "." + fileExtension
    let newFileUrl = fileUrl.deletingLastPathComponent().appendingPathComponent(newFileName)

    if fileManager.fileExists(atPath: newFileUrl.path) {
      return generateNewFileUrl(for: newFileUrl)
    } else {
      return newFileUrl
    }
  }
}

// MARK: - CloudStorageManger Observing
extension CloudStorageManager {
  func addObserver(_ observer: AnyObject, onErrorCompletionHandler: @escaping (NSError?) -> Void) {
    let id = ObjectIdentifier(observer)
    observers[id] = Observation(observer: observer, onErrorCompletionHandler:onErrorCompletionHandler)
    // Notify the new observer immediately to handle initial state.
    observers[id]?.onErrorCompletionHandler?(synchronizationError as NSError?)
  }

  func addObserver(_ observer: AnyObject, onSynchronizationIsInProcessCompletionHandler: @escaping (Bool) -> Void) {
    let id = ObjectIdentifier(observer)
    observers[id] = Observation(observer: observer, onSynchronizationIsInProcessCompletionHandler: onSynchronizationIsInProcessCompletionHandler)
    // Notify the new observer immediately to handle initial state.
    observers[id]?.onSynchronizationIsInProcessCompletionHandler?(synchronizationIsInProcess)
  }

  func removeObserver(_ observer: AnyObject) {
    let id = ObjectIdentifier(observer)
    observers.removeValue(forKey: id)
  }

  private func notifyObserversOnSynchronizationError(_ error: SynchronizationError?) {
    self.observers.removeUnreachable().forEach { _, observable in
      DispatchQueue.main.async {
        observable.onErrorCompletionHandler?(error as NSError?)
      }
    }
  }

  private func notifyObserversOnSynchronizationIsInProcess(_ isInProcess: Bool) {
    self.observers.removeUnreachable().forEach { _, observable in
      DispatchQueue.main.async {
        observable.onSynchronizationIsInProcessCompletionHandler?(isInProcess)
      }
    }
  }
}

// MARK: - BookmarksObserver
extension CloudStorageManager: BookmarksObserver {
  func onBookmarksLoadFinished() {
    LOG(.debug, "Bookmarks load finished. Signalling semaphore...")
    semaphore?.signal()
  }
}

// MARK: - Extend background time execution
private extension CloudStorageManager {
  func extendBackgroundExecutionIfNeeded(expirationHandler: (() -> Void)? = nil) {
    guard synchronizationIsInProcess else {
      expirationHandler?()
      return
    }
    backgroundTaskIdentifier = UIApplication.shared.beginBackgroundTask(withName: nil) { [weak self] in
      guard let self else { return }
      expirationHandler?()
      self.cancelBackgroundExecutionIfNeeded()
    }
  }

  func cancelBackgroundExecutionIfNeeded() {
    guard backgroundTaskIdentifier != .invalid else { return }
    DispatchQueue.main.async { [weak self] in
      guard let self else { return }
      UIApplication.shared.endBackgroundTask(self.backgroundTaskIdentifier)
      self.backgroundTaskIdentifier = UIBackgroundTaskIdentifier.invalid
    }
  }
}

// MARK: - FileManager + Directories
extension FileManager {
  var bookmarksDirectoryUrl: URL {
    urls(for: .documentDirectory, in: .userDomainMask).first!.appendingPathComponent(kBookmarksDirectoryName, isDirectory: true)
  }

  func trashDirectoryUrl(for baseDirectoryUrl: URL) throws -> URL {
    let trashDirectory = baseDirectoryUrl.appendingPathComponent(kTrashDirectoryName, isDirectory: true)
    if !fileExists(atPath: trashDirectory.path) {
      try createDirectory(at: trashDirectory, withIntermediateDirectories: true)
    }
    return trashDirectory
  }
}

// MARK: - Notification + iCloudSynchronizationDidChangeEnabledState
extension Notification.Name {
  static let iCloudSynchronizationDidChangeEnabledStateNotification = Notification.Name(kICloudSynchronizationDidChangeEnabledStateNotificationName)
}

@objc extension NSNotification {
  public static let iCloudSynchronizationDidChangeEnabledState = Notification.Name.iCloudSynchronizationDidChangeEnabledStateNotification
}

// MARK: - URL + ResourceValues
private extension URL {
  mutating func setResourceModificationDate(_ date: Date) throws {
    var resource = try resourceValues(forKeys:[.contentModificationDateKey])
    resource.contentModificationDate = date
    try setResourceValues(resource)
  }
}

private extension Data {
  func write(to url: URL, options: Data.WritingOptions = .atomic, lastModificationDate: TimeInterval? = nil) throws {
    var url = url
    try write(to: url, options: options)
    if let lastModificationDate {
      try url.setResourceModificationDate(Date(timeIntervalSince1970: lastModificationDate))
    }
  }
}

// MARK: - Dictionary + RemoveUnreachable
private extension Dictionary where Key == ObjectIdentifier, Value == CloudStorageManager.Observation {
  mutating func removeUnreachable() -> Self {
    for (id, observation) in self {
      if observation.observer == nil {
        removeValue(forKey: id)
      }
    }
    return self
  }
}
