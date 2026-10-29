import QtQuick
import Murasaki.App

// The screen and desktop a page applies something to, with the choices the compositor offers. Not visual: a page
// binds its combo boxes to outputChoices/desktopChoices and sets outputWanted/desktopWanted from them.
QtObject {
    id: state

    property var outputNames: []
    property var desktopList: []
    // What the user picked; the effective target falls back to "*" while the pick is not available.
    property string outputWanted: "*"
    property string desktopWanted: "*"

    readonly property var outputChoices: [{text: qsTr("All screens"), value: "*"}].concat(
        outputNames.map(n => ({text: plain(n), value: n})))
    readonly property var desktopChoices: [{text: qsTr("All desktops"), value: "*"}].concat(
        desktopList.map(d => ({text: plain(d.name) || qsTr("Desktop %1").arg(d.number ?? ""), value: d.id})))
    readonly property string output: outputChoices.some(c => c.value === outputWanted) ? outputWanted : "*"
    readonly property string desktop: desktopChoices.some(c => c.value === desktopWanted) ? desktopWanted : "*"

    function plain(s) {
        return s === undefined || s === null ? "" : String(s).replace(/</g, "‹").replace(/>/g, "›");
    }

    function nameOf(outputId, desktopId) {
        const screen = outputId === "*" ? qsTr("All screens") : outputId;
        const desk = desktopId === "*" ? qsTr("All desktops")
            : (desktopList.find(d => d.id === desktopId)?.name ?? desktopId);
        return desktopId === "*" ? screen : qsTr("%1, %2").arg(screen).arg(desk);
    }

    function refresh() {
        const names = Surface.outputs.map(o => o.name);
        if (JSON.stringify(names) !== JSON.stringify(outputNames))
            outputNames = names;
        const desktops = Surface.desktops.map(d => ({id: d.id, name: d.name}));
        if (JSON.stringify(desktops) !== JSON.stringify(desktopList))
            desktopList = desktops;
    }

    property Connections surface: Connections {
        target: Surface
        function onOutputsChanged() { state.refresh(); }
        function onDesktopsChanged() { state.refresh(); }
    }

    Component.onCompleted: refresh()
}
