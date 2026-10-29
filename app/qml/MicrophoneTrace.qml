/*
 * SPDX-FileCopyrightText: 2026 Kevin Hahn <kevin_hahn@sil.org>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

/**
 * Details and a scrolling level trace for a streaming microphone, plus the plugin's settings,
 * shown below the "Microphone" row of the device page. Only the settings are visible while idle.
 */
Item {
    id: root

    required property MicrophoneInfo info

    property real elapsedMs: 0

    implicitHeight: layout.implicitHeight + Kirigami.Units.largeSpacing

    function formatElapsed(ms: real): string {
        const totalSeconds = Math.max(0, Math.floor(ms / 1000));
        const minutes = Math.floor(totalSeconds / 60);
        const seconds = totalSeconds % 60;
        return minutes + ":" + (seconds < 10 ? "0" + seconds : seconds);
    }

    Timer {
        interval: 1000
        repeat: true
        triggeredOnStart: true
        running: root.info.streaming
        onTriggered: root.elapsedMs = root.info.startedAt > 0 ? Date.now() - root.info.startedAt : 0
    }

    ColumnLayout {
        id: layout

        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
            leftMargin: Kirigami.Units.largeSpacing * 2
            rightMargin: Kirigami.Units.largeSpacing * 2
        }
        spacing: Kirigami.Units.smallSpacing

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing
            visible: root.info.streaming

            QQC2.Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                color: Kirigami.Theme.disabledTextColor
                text: {
                    const base = i18nd("kdeconnect-app", "%1 Hz mono - %2 - peak %3 dB", root.info.sampleRate, root.formatElapsed(root.elapsedMs), Math.round(root.info.peakDb));
                    return root.info.outputMode ? base + " - " + root.info.outputMode : base;
                }
            }

            QQC2.Button {
                text: i18nd("kdeconnect-app", "Stop")
                icon.name: "media-playback-stop"
                onClicked: root.info.stop()
            }
        }

        Canvas {
            id: canvas

            Layout.fillWidth: true
            Layout.preferredHeight: 44
            visible: root.info.streaming

            readonly property int barGap: 2
            readonly property color barColor: Kirigami.Theme.highlightColor
            readonly property color baselineColor: Kirigami.Theme.textColor

            onBarColorChanged: requestPaint()
            onBaselineColorChanged: requestPaint()

            Connections {
                target: root.info

                function onLevelsChanged(): void {
                    canvas.requestPaint();
                }
            }

            onPaint: {
                const ctx = getContext("2d");
                ctx.clearRect(0, 0, width, height);

                ctx.globalAlpha = 0.3;
                ctx.fillStyle = baselineColor;
                ctx.fillRect(0, height - 1, width, 1);

                const levels = root.info.levels;
                const count = levels.length;
                if (count === 0) {
                    return;
                }
                const barWidth = Math.max(1, (width - barGap * (count - 1)) / count);
                const maxBarHeight = height - 2;
                ctx.fillStyle = barColor;
                for (let i = 0; i < count; ++i) {
                    // Decibel scale with a -50 dB floor: quiet sounds stay visible and the top few dB
                    // all fill the bar, which reads better than a linear meter for speech.
                    const db = levels[i] > 0 ? 20 * Math.log10(levels[i]) : -100;
                    const scaled = Math.min(1, Math.max(0, (db + 50) / 50));
                    const barHeight = Math.max(1, Math.round(scaled * maxBarHeight));
                    // Newest bar on the right at full opacity, fading toward the left
                    ctx.globalAlpha = 0.25 + 0.75 * (i / Math.max(1, count - 1));
                    ctx.fillRect(i * (barWidth + barGap), height - 1 - barHeight, barWidth, barHeight);
                }
            }
        }

        QQC2.CheckBox {
            Layout.fillWidth: true
            text: i18nd("kdeconnect-app", "Exclusive device access (lowest latency, applies at the next start)")
            checked: root.info.exclusiveMode
            onToggled: root.info.setExclusiveMode(checked)
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing

            QQC2.Label {
                text: i18nd("kdeconnect-app", "Gain")
            }

            QQC2.Slider {
                id: gainSlider

                Layout.fillWidth: true
                from: 0
                to: 24
                stepSize: 1
                value: root.info.gainDb
                // Applies live while dragging; the binding above is restored from the daemon's value
                onMoved: root.info.setGainDb(value)
            }

            QQC2.Label {
                id: gainLabel

                Layout.minimumWidth: gainLabelMetrics.width
                horizontalAlignment: Text.AlignRight
                font.features: ({ "tnum": 1 })
                text: i18nd("kdeconnect-app", "+%1 dB", Math.round(gainSlider.value))

                TextMetrics {
                    id: gainLabelMetrics
                    font: gainLabel.font
                    text: i18nd("kdeconnect-app", "+%1 dB", 24)
                }
            }
        }
    }
}
