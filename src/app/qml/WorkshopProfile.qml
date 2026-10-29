import QtQuick
import QtWebEngine

// The Workshop's browser profile. Main.qml holds the only instance for the whole run, and the Workshop pages
// (which come and go) use `instance()`: a profile must outlive its views, and two profiles must not share a
// storage name. A named profile is stored on disk: the login cookies live under
// ~/.local/share/uriel2121ger-art/murasaki/QtWebEngine/murasaki-workshop (organization/application name) and the
// HTTP cache under the same path in ~/.cache.
WebEngineProfilePrototype {
    storageName: "murasaki-workshop"
    persistentCookiesPolicy: WebEngineProfile.ForcePersistentCookies
    httpCacheType: WebEngineProfile.DiskHttpCache
    // Site permissions (camera, location, notifications, ...) are denied by the page and never stored.
    persistentPermissionsPolicy: WebEngineProfile.AskEveryTime
}
