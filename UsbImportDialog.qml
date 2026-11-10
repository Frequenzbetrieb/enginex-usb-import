import QtQuick 2.15
import Planck 1.0

Item {
    id: usbImportRoot
    property bool importInProgress: false
    property bool waitingForResult: false
    property string importingDrive: ""
    property int watchdogMisses: 0

    Timer {
        id: pollTimer
        interval: 3000
        running: true
        repeat: true
        onTriggered: {
            if (usbImportRoot.waitingForResult) {
                checkProgress()
            } else if (!usbImportRoot.importInProgress) {
                checkNewDrive()
            }
        }
    }

    function checkNewDrive() {
        var xhr = new XMLHttpRequest()
        xhr.open("GET", "http://127.0.0.1:8082/api/status")
        xhr.timeout = 2000
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE && xhr.status === 200) {
                try {
                    var data = JSON.parse(xhr.responseText)
                    if (data.select_target) {
                        showTargetPrompt(data.candidates, 0)
                    } else if (data.new_drive) {
                        showImportDialog(data.new_drive)
                    }
                } catch(e) {}
            }
        }
        xhr.send()
    }

    function showTargetPrompt(candidates, index) {
        if (index >= candidates.length) {
            return
        }
        importInProgress = true
        var isLast = (index === candidates.length - 1)
        var options = [
            { text: "Yes, use this", role: Prompt.AcceptRole }
        ]
        if (!isLast) {
            options.push({ text: "No, next", role: Prompt.RejectRole })
        } else {
            options.push({ text: "Cancel", role: Prompt.RejectRole })
        }

        var prompt = PlanckGUI.views.prompts.post({
            title: "Select Target Drive",
            message: "Is '" + candidates[index] + "' your internal drive?\n\nImported music will be stored here.",
            options: options
        })
        prompt.finished.connect(function(role) {
            if (role === Prompt.AcceptRole) {
                showRememberPrompt(candidates[index])
            } else if (!isLast) {
                showTargetPrompt(candidates, index + 1)
            } else {
                importInProgress = false
            }
        })
    }

    function showRememberPrompt(driveName) {
        var prompt = PlanckGUI.views.prompts.post({
            title: "Remember Choice?",
            message: "Save '" + driveName + "' as target drive?\n\nReset later: http://<deck-ip>:8082/api/reset-target",
            options: [
                { text: "Save", role: Prompt.AcceptRole },
                { text: "Just this time", role: Prompt.RejectRole }
            ]
        })
        prompt.finished.connect(function(role) {
            selectTarget(driveName, role === Prompt.AcceptRole)
            importInProgress = false
        })
    }

    function selectTarget(driveName, remember) {
        var xhr = new XMLHttpRequest()
        xhr.open("POST", "http://127.0.0.1:8082/api/select-target")
        xhr.setRequestHeader("Content-Type", "application/json")
        xhr.send(JSON.stringify({drive: driveName, remember: remember}))
    }

    function showImportDialog(driveName) {
        importInProgress = true
        importingDrive = driveName
        var prompt = PlanckGUI.views.prompts.post({
            title: "USB Drive Detected",
            message: "'" + driveName + "' was connected.\n\nImport music files to internal drive?",
            options: [
                { text: "Import", role: Prompt.AcceptRole },
                { text: "Ignore", role: Prompt.RejectRole }
            ]
        })
        prompt.finished.connect(function(role) {
            if (role === Prompt.AcceptRole) {
                startImport(driveName)
            } else {
                dismissDrive(driveName)
                importInProgress = false
                importingDrive = ""
            }
        })
    }

    function startImport(driveName) {
        watchdogMisses = 0
        PlanckGUI.views.prompts.post({
            id: "UsbImporting",
            title: "Importing...",
            message: "Scanning '" + driveName + "' for music files...",
            options: []
        })
        var xhr = new XMLHttpRequest()
        xhr.open("POST", "http://127.0.0.1:8082/api/import")
        xhr.setRequestHeader("Content-Type", "application/json")
        xhr.timeout = 5000
        xhr.onerror = function() {
            PlanckGUI.views.prompts.drop("UsbImporting")
            showError("Could not start import — daemon not responding.")
        }
        xhr.ontimeout = function() {
            PlanckGUI.views.prompts.drop("UsbImporting")
            showError("Import request timed out.")
        }
        xhr.send(JSON.stringify({drive: driveName}))
        waitingForResult = true
        pollTimer.interval = 1500
    }

    function checkProgress() {
        var xhr = new XMLHttpRequest()
        xhr.open("GET", "http://127.0.0.1:8082/api/status")
        xhr.timeout = 2000
        xhr.onerror = function() { handleWatchdog() }
        xhr.ontimeout = function() { handleWatchdog() }
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                if (xhr.status !== 200) {
                    handleWatchdog()
                    return
                }
                watchdogMisses = 0
                try {
                    var data = JSON.parse(xhr.responseText)
                    if (data.importing) {
                        var msg = ""
                        if (data.files_total > 0) {
                            var done = data.files_copied + data.files_skipped + data.files_failed
                            msg = "Copying " + done + " / " + data.files_total
                            if (data.files_failed > 0)
                                msg += " (" + data.files_failed + " failed)"
                            if (data.current_file)
                                msg += "\n" + data.current_file
                        } else {
                            msg = "Scanning for music files..."
                        }
                        PlanckGUI.views.prompts.drop("UsbImporting")
                        PlanckGUI.views.prompts.post({
                            id: "UsbImporting",
                            title: "Importing...",
                            message: msg,
                            options: []
                        })
                    } else if (data.done) {
                        waitingForResult = false
                        pollTimer.interval = 3000
                        PlanckGUI.views.prompts.drop("UsbImporting")
                        ackDone()

                        if (data.error && data.error.length > 0) {
                            importInProgress = false
                            importingDrive = ""
                            showError(data.error)
                        } else if (data.restart_pending) {
                            showRestartDialog(data)
                        } else {
                            importInProgress = false
                            importingDrive = ""
                            var msg2 = buildResultMessage(data)
                            PlanckGUI.views.prompts.post({
                                title: "Import Complete",
                                message: msg2,
                                options: [{ text: "OK", role: Prompt.AcceptRole }]
                            })
                        }
                    } else if (data.idle) {
                        waitingForResult = false
                        pollTimer.interval = 3000
                        PlanckGUI.views.prompts.drop("UsbImporting")
                        importInProgress = false
                        importingDrive = ""
                        showError("Import was interrupted unexpectedly.")
                    }
                } catch(e) {}
            }
        }
        xhr.send()
    }

    function handleWatchdog() {
        watchdogMisses++
        if (watchdogMisses >= 5) {
            waitingForResult = false
            pollTimer.interval = 3000
            PlanckGUI.views.prompts.drop("UsbImporting")
            importInProgress = false
            importingDrive = ""
            showError("Lost connection to import daemon.")
            watchdogMisses = 0
        }
    }

    function showRestartDialog(data) {
        var msg = buildResultMessage(data)
        msg += "\n\nRestart EngineX to see imported tracks?\nPlayback will stop."
        var restartPrompt = PlanckGUI.views.prompts.post({
            title: "Import Complete",
            message: msg,
            options: [
                { text: "Restart Now", role: Prompt.AcceptRole },
                { text: "Later", role: Prompt.RejectRole }
            ]
        })
        restartPrompt.finished.connect(function(role) {
            importInProgress = false
            importingDrive = ""
            if (role === Prompt.AcceptRole) {
                restartEngine()
            } else {
                notifyLater()
            }
        })
    }

    function buildResultMessage(data) {
        var msg = ""
        if (data.files_copied > 0)
            msg += data.files_copied + " files copied"
        if (data.files_skipped > 0) {
            if (msg.length > 0) msg += ", "
            msg += data.files_skipped + " already existed"
        }
        if (data.files_failed > 0) {
            if (msg.length > 0) msg += ", "
            msg += data.files_failed + " failed"
        }
        if (msg.length === 0)
            msg = "No music files found."
        else
            msg += "\nTracks registered in collection."
        return msg
    }

    function showError(message) {
        importInProgress = false
        importingDrive = ""
        PlanckGUI.views.prompts.post({
            title: "Import Failed",
            message: message,
            options: [{ text: "OK", role: Prompt.AcceptRole }]
        })
    }

    function dismissDrive(driveName) {
        var xhr = new XMLHttpRequest()
        xhr.open("POST", "http://127.0.0.1:8082/api/dismiss")
        xhr.setRequestHeader("Content-Type", "application/json")
        xhr.send(JSON.stringify({drive: driveName}))
    }

    function ackDone() {
        var xhr = new XMLHttpRequest()
        xhr.open("POST", "http://127.0.0.1:8082/api/ack")
        xhr.send()
    }

    function restartEngine() {
        var xhr = new XMLHttpRequest()
        xhr.open("POST", "http://127.0.0.1:8082/api/restart")
        xhr.send()
    }

    function notifyLater() {
        var xhr = new XMLHttpRequest()
        xhr.open("POST", "http://127.0.0.1:8082/api/later")
        xhr.send()
    }
}
