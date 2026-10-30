import QtQuick
import QtQuick.Window
import QtWebEngine

// One native window hosting one web page. C++ (windows.cpp) owns the lifecycle, geometry and input region.
Window {
    id: root
    property string label
    property url startUrl
    property string origin
    property QtObject shared   // qml/App.qml: web channel + profile
    property bool pet: false
    signal pageLoaded()
    signal externalUrl(url url)

    color: "transparent"
    flags: Qt.FramelessWindowHint

    WebEngineView {
        id: view
        objectName: "view"
        anchors.fill: parent
        backgroundColor: "transparent"
        webChannel: root.shared.channel
        url: root.startUrl
        settings.javascriptCanAccessClipboard: false
        settings.localContentCanAccessRemoteUrls: false
        settings.focusOnNavigationEnabled: false
        settings.playbackRequiresUserGesture: false
        // WebEngineScript is a value type in Qt 6: plain objects, not elements.
        userScripts.collection: [
            { name: "qwebchannel", sourceUrl: "qrc:///qtwebchannel/qwebchannel.js", injectionPoint: WebEngineScript.DocumentCreation, worldId: WebEngineScript.MainWorld },
            { name: "label", sourceCode: "window.__WW_LABEL__=" + JSON.stringify(root.label) + ";window.__WW_DEBUG__=" + (Qt.application.arguments.indexOf("--debug-js") >= 0) + ";", injectionPoint: WebEngineScript.DocumentCreation, worldId: WebEngineScript.MainWorld },
            { name: "tauri", sourceUrl: "qrc:/js/tauri.js", injectionPoint: WebEngineScript.DocumentCreation, worldId: WebEngineScript.MainWorld },
            { name: "ws", sourceUrl: "qrc:/js/ws.js", injectionPoint: WebEngineScript.DocumentCreation, worldId: WebEngineScript.MainWorld }
        ].concat(root.pet ? [
            { name: "pet", sourceUrl: "qrc:/js/pet.js", injectionPoint: WebEngineScript.DocumentReady, worldId: WebEngineScript.MainWorld }
        ] : [])
        onLoadingChanged: (req) => { if (req.status === WebEngineView.LoadSucceededStatus) root.pageLoaded() }
        // The page may only navigate inside the app origin; anything else opens in the user's browser.
        Timer {  // --debug-js: DOM/visibility/frame-rate probe (a blank window with rAF=0 means the compositor delivers no frames)
            interval: 7000; running: Qt.application.arguments.indexOf("--debug-js") >= 0; repeat: false
            onTriggered: view.runJavaScript("(()=>{let n=0;const t0=performance.now();const f=()=>{n++;if(performance.now()-t0<1000)requestAnimationFrame(f);else console.log('[probe-raf]',n)};requestAnimationFrame(f)})();JSON.stringify({vis:document.visibilityState,focus:document.hasFocus(),online:navigator.onLine,gate:document.documentElement.hasAttribute('data-ww-gate'),op:getComputedStyle(document.documentElement).opacity,len:document.body.innerText.length,text:document.body.innerText.slice(0,120),w:innerWidth,h:innerHeight,dpr:devicePixelRatio,kids:document.querySelectorAll('*').length})", function (r) { console.log("[probe:" + root.label + "]", r) })
        }
        onNavigationRequested: (req) => {
            if (String(req.url).indexOf(root.origin) !== 0 && String(req.url).indexOf("data:") !== 0 && String(req.url) !== "about:blank") {
                req.action = WebEngineNavigationRequest.IgnoreRequest
                root.externalUrl(req.url)
            }
        }
        onNewWindowRequested: (req) => root.externalUrl(req.requestedUrl)
        onJavaScriptConsoleMessage: (level, message, line, source) => {
            if (Qt.application.arguments.indexOf("--debug-js") >= 0) console.log("[js:" + root.label + "]", message, source + ":" + line)
        }
    }
}
