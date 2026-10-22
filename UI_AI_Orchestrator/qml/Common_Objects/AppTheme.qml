import QtQuick

QtObject {
    id: theme

    required property string mode
    required property real scaleFactor
    property bool compactMode: false
    property bool reduceAnimations: false

    readonly property bool isLightMode: mode === "light"
    readonly property bool isDarkMode: mode === "dark"
    readonly property bool isTransparentMode: mode === "transparent"

    function dp(value) {
        return Math.round(value * scaleFactor * 100) / 100
    }

    readonly property color windowBackground: isLightMode
                                              ? "#F4F7FB"
                                              : isDarkMode
                                                ? "#0B1120"
                                                : Qt.rgba(0.035, 0.055, 0.09, 0.84)
    readonly property color surface: isLightMode
                                     ? "#FFFFFF"
                                     : isDarkMode
                                       ? "#111827"
                                       : Qt.rgba(0.08, 0.11, 0.17, 0.72)
    readonly property color surfaceRaised: isLightMode
                                           ? "#FFFFFF"
                                           : isDarkMode
                                             ? "#172033"
                                             : Qt.rgba(0.12, 0.16, 0.24, 0.78)
    readonly property color surfaceAlternative: isLightMode
                                                ? "#EAF0F7"
                                                : isDarkMode
                                                  ? "#182235"
                                                  : Qt.rgba(0.16, 0.20, 0.29, 0.58)
    readonly property color overlay: isLightMode
                                     ? Qt.rgba(0.06, 0.09, 0.16, 0.38)
                                     : Qt.rgba(0.0, 0.0, 0.0, 0.58)

    readonly property color textPrimary: isLightMode ? "#111827" : "#F8FAFC"
    readonly property color textSecondary: isLightMode ? "#4B5563" : "#B5C0D0"
    readonly property color textMuted: isLightMode ? "#7B8798" : "#7F8DA3"
    readonly property color textDisabled: isLightMode ? "#A7B0BE" : "#59667A"
    readonly property color textOnAccent: "#FFFFFF"

    readonly property color border: isLightMode
                                    ? "#D8E0EA"
                                    : isDarkMode
                                      ? "#29364B"
                                      : Qt.rgba(1.0, 1.0, 1.0, 0.16)
    readonly property color borderStrong: isLightMode
                                          ? "#B8C3D1"
                                          : isDarkMode
                                            ? "#3B4A61"
                                            : Qt.rgba(1.0, 1.0, 1.0, 0.26)
    readonly property color separator: isLightMode
                                       ? "#E2E8F0"
                                       : isDarkMode
                                         ? "#202C40"
                                         : Qt.rgba(1.0, 1.0, 1.0, 0.10)

    readonly property color accent: isLightMode ? "#4F46E5" : "#7C83FF"
    readonly property color accentHover: isLightMode ? "#4338CA" : "#9297FF"
    readonly property color accentPressed: isLightMode ? "#3730A3" : "#6268E8"
    readonly property color accentSoft: isLightMode
                                        ? "#E7E7FF"
                                        : Qt.rgba(0.49, 0.51, 1.0, 0.18)
    readonly property color focusRing: isLightMode ? "#818CF8" : "#A5B4FC"

    readonly property color buttonBackground: surfaceAlternative
    readonly property color buttonHover: isLightMode
                                         ? "#DEE7F1"
                                         : isDarkMode
                                           ? "#223047"
                                           : Qt.rgba(1.0, 1.0, 1.0, 0.14)
    readonly property color buttonPressed: isLightMode
                                           ? "#D1DCE8"
                                           : isDarkMode
                                             ? "#2A3951"
                                             : Qt.rgba(1.0, 1.0, 1.0, 0.20)

    readonly property color inputBackground: isLightMode
                                            ? "#FFFFFF"
                                            : isDarkMode
                                              ? "#0F1726"
                                              : Qt.rgba(0.03, 0.05, 0.09, 0.48)
    readonly property color inputBorder: border
    readonly property color inputBorderFocused: accent
    readonly property color selection: accent

    readonly property color popupBackground: surfaceRaised
    readonly property color tooltipBackground: isLightMode ? "#1F2937" : "#202B3E"
    readonly property color tooltipText: "#FFFFFF"

    readonly property color success: isLightMode ? "#16865B" : "#45D39A"
    readonly property color warning: isLightMode ? "#B66A06" : "#F5B94C"
    readonly property color error: isLightMode ? "#C63D4F" : "#FF7185"
    readonly property color successSoft: isLightMode
                                         ? Qt.rgba(0.09, 0.52, 0.36, 0.14)
                                         : Qt.rgba(0.27, 0.83, 0.60, 0.16)
    readonly property color warningSoft: isLightMode
                                         ? Qt.rgba(0.71, 0.42, 0.02, 0.14)
                                         : Qt.rgba(0.96, 0.73, 0.30, 0.16)
    readonly property color errorSoft: isLightMode
                                       ? Qt.rgba(0.78, 0.24, 0.31, 0.12)
                                       : Qt.rgba(1.0, 0.44, 0.52, 0.16)
    readonly property color information: isLightMode ? "#2563EB" : "#60A5FA"

    readonly property color link: isLightMode ? "#4338CA" : "#9CA3FF"
    readonly property color linkVisited: isLightMode ? "#7C3AED" : "#C4A7FF"
    readonly property color shadow: isLightMode
                                    ? Qt.rgba(0.06, 0.09, 0.16, 0.18)
                                    : Qt.rgba(0.0, 0.0, 0.0, 0.48)

    readonly property real cornerRadiusSmall: dp(8)
    readonly property real cornerRadiusMedium: dp(12)
    readonly property real cornerRadiusLarge: dp(18)
    readonly property real titleBarHeight: dp(42)
    readonly property real windowResizeMargin: Math.max(5, dp(6))
    readonly property int animationDuration: reduceAnimations ? 0 : 150
    readonly property real controlHeight: dp(compactMode ? 34 : 40)
    readonly property real controlSpacing: dp(compactMode ? 6 : 8)
    readonly property real contentMargin: dp(compactMode ? 18 : 24)
}
