/*
 * SPDX-FileCopyrightText: 2026 Kevin Hahn <kevin_hahn@sil.org>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

pragma ComponentBehavior: Bound

import QtQuick

import org.kde.kdeconnect as KDEConnect

QtObject {
    id: root

    required property KDEConnect.DeviceDbusInterface device

    // Number of 50 ms level windows kept for the trace: 5 seconds
    readonly property int historyLength: 100

    readonly property alias available: checker.available
    readonly property bool streaming: microphone?.streaming ?? false
    readonly property string outputDevice: microphone?.outputDevice ?? ""
    readonly property int sampleRate: microphone?.sampleRate ?? 0
    readonly property double startedAt: microphone?.startedAt ?? 0
    readonly property string iconName: streaming ? "audio-input-microphone" : "microphone-sensitivity-muted"

    // Peak of the most recent level window in dBFS, clamped at -60
    property real peakDb: -60
    // The last historyLength peak levels (0..1), oldest first
    property var levels: emptyLevels()

    readonly property string displayString: {
        if (streaming) {
            return i18nd("kdeconnect-app", "Microphone: streaming to %1", outputDevice);
        } else {
            return i18nd("kdeconnect-app", "Microphone: not streaming - start it from the phone");
        }
    }

    readonly property KDEConnect.PluginChecker pluginChecker: KDEConnect.PluginChecker {
        id: checker
        pluginName: "microphone"
        device: root.device
    }

    property KDEConnect.MicrophoneDbusInterface microphone

    readonly property Connections levelConnection: Connections {
        target: root.microphone

        function onLevelChangedProxy(peak: real, rms: real): void {
            root.peakDb = Math.max(-60, 20 * Math.log10(peak));
            root.levels = root.levels.slice(1).concat([peak]);
        }
    }

    function emptyLevels() {
        return new Array(historyLength).fill(0);
    }

    function stop(): void {
        if (microphone) {
            microphone.stop();
        }
    }

    onStreamingChanged: {
        if (!streaming) {
            peakDb = -60;
            levels = emptyLevels();
        }
    }

    onAvailableChanged: {
        if (available && device) {
            microphone = KDEConnect.MicrophoneDbusInterfaceFactory.create(device.id());
        } else {
            microphone = null;
        }
    }
}
