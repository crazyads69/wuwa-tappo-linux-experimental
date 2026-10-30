import QtQuick
import QtWebEngine
import QtWebChannel

// Objects shared by every window: the web channel (the IPC bridge). All views use the default profile, which is persistent
// (cookies and localStorage live under ~/.local/share/com.kuro.wuwatappo.global/QtWebEngine).
QtObject {
    property WebChannel channel: WebChannel { }
}
