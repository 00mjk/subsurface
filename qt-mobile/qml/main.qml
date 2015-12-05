import QtQuick 2.4
import QtQuick.Controls 1.2
import QtQuick.Controls.Styles 1.2
import QtQuick.Window 2.2
import QtQuick.Dialogs 1.2
import QtQuick.Layouts 1.1
import QtQuick.Window 2.2
import org.subsurfacedivelog.mobile 1.0
import org.kde.plasma.mobilecomponents 0.2 as MobileComponents

MobileComponents.ApplicationWindow {
	title: qsTr("Subsurface mobile")
	property bool fullscreen: true

	FontMetrics {
		id: fontMetrics
	}

	visible: true

	globalDrawer: MobileComponents.GlobalDrawer{
		title: "Subsurface"
		titleIcon: "qrc:/qml/subsurface-mobile-icon.png"

		bannerImageSource: "dive.jpg"
		actions: [
		Action {
			text: "Preferences"
			onTriggered: {
				stackView.push(prefsWindow)
			}
		},

		Action {
			text: "Cloud login credentials"
			onTriggered: {
				stackView.push(cloudCredWindow)
			}
		},

		Action {
			text: "Load dives from cloud"
			onTriggered: {
				manager.loadDives();
			}
		},

		Action {
			text: "Download dives from dive computer"
			onTriggered: {
				stackView.push(downloadDivesWindow)
			}
		},

		Action {
			text: "Add dive"
			onTriggered: {
				manager.addDive();
				stackView.push(detailsWindow)
			}
		},

		Action {
			text: "Save changes"
			onTriggered: {
				manager.saveChanges();
			}
		},

		MobileComponents.ActionGroup {
			text: "GPS"
		Action {
				text: "Apply GPS data to dives"
				onTriggered: {
						manager.applyGpsData();
				}
		}

		Action {
				text: "Send GPS data to server"
				onTriggered: {
						manager.sendGpsData();
				}
		}

		Action {
				text: "Clear stored GPS data"
				onTriggered: {
						manager.clearGpsData();
				}
		}
	},

		Action {
			text: "View log"
			onTriggered: {
				stackView.push(logWindow)
			}
		},

		Action {
			text: "Theme information"
			onTriggered: {
				stackView.push(themetest)
			}
		}
	    ]
		MouseArea {
			height: childrenRect.height
			width: MobileComponents.Units.gridUnit * 10
			CheckBox {
				//text: "Run location service"
				id: locationCheckbox
				anchors {
					left: parent.left
					top: parent.top
				}
				checked: manager.locationServiceEnabled
				onCheckedChanged: {
					manager.locationServiceEnabled = checked;
				}
			}
			MobileComponents.Label {
				x: MobileComponents.Units.gridUnit * 1.5
				anchors {
					left: locationCheckbox.right
					//leftMargin: units.smallSpacing
					verticalCenter: locationCheckbox.verticalCenter
				}
				text: "Run location service"
			}
			onClicked: {
				print("Click.")
				locationCheckbox.checked = !locationCheckbox.checked
			}
		}
	}

	QtObject {
		id: subsurfaceTheme
		property int titlePointSize: Math.round(fontMetrics.font.pointSize * 1.5)
		property int smallPointSize: Math.round(fontMetrics.font.pointSize * 0.8)
		property color accentColor: "#2d5b9a"
		property color accentTextColor: "#ececec"
	}

	toolBar: TopBar {
		width: parent.width
		height: Layout.minimumHeight
	}

	property Item stackView: pageStack
	initialPage: DiveList {
		anchors.fill: detailsPage
		id: diveDetails
	}

	QMLManager {
		id: manager
	}

	Preferences {
		id: prefsWindow
		visible: false
	}

	CloudCredentials {
		id: cloudCredWindow
		visible: false
	}

	DiveDetails {
		id: detailsWindow
		visible: false
	}

	DownloadFromDiveComputer {
		id: downloadDivesWindow
		visible: false
	}

	Log {
		id: logWindow
		visible: false
	}

	ThemeTest {
		id: themetest
		visible: false
	}

	Component.onCompleted: {
		print("MobileComponents.Units.gridUnit is: " + MobileComponents.Units.gridUnit);
		manager.finishSetup();
	}
}
