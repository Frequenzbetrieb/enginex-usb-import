import QtQuick 2.15
import Planck 1.0

Item {
    id: root
    property bool importInProgress: false
    property bool waitingForResult: false
    property string importingDrive: ""
    property int watchdogMisses: 0
    property string pendingDrive: ""

    Timer {
        id: pollTimer
        interval: 3000
        running: true
        repeat: true
        onTriggered: {
            if (root.waitingForResult)
                checkProgress()
            else if (!root.importInProgress)
                checkNewDrive()
        }
    }

    /* Step 1: Poll for new USB drive */
    function checkNewDrive() {
        var xhr = new XMLHttpRequest()
        xhr.open("GET", "http://127.0.0.1:8082/api/status")
        xhr.timeout = 2000
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE && xhr.status === 200) {
                try {
                    var data = JSON.parse(xhr.responseText)
                    if (data.new_drive)
                        askImport(data.new_drive)
                } catch(e) {}
            }
        }
        xhr.send()
    }

    /* Step 1: "Import music?" Yes/No */
    function askImport(driveName) {
        importInProgress = true
        pendingDrive = driveName
        var prompt = PlanckGUI.views.prompts.post({
            title: "USB Drive Detected",
            message: "'" + driveName + "' connected.\n\nImport music to internal drive?",
            options: [
                { text: "Import", role: Prompt.AcceptRole },
                { text: "Ignore", role: Prompt.RejectRole }
            ]
        })
        prompt.finished.connect(function(role) {
            if (role === Prompt.AcceptRole)
                tryStartImport(driveName)
            else {
                dismissDrive(driveName)
                importInProgress = false
                pendingDrive = ""
            }
        })
    }

    /* Step 2: Try to start import — server returns needs_target if no target set */
    function tryStartImport(driveName) {
        var xhr = new XMLHttpRequest()
        xhr.open("POST", "http://127.0.0.1:8082/api/import")
        xhr.setRequestHeader("Content-Type", "application/json")
        xhr.timeout = 5000
        xhr.onerror = function() {
            showError("Daemon not responding.")
        }
        xhr.ontimeout = function() {
            showError("Import request timed out.")
        }
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE && xhr.status === 200) {
                try {
                    var data = JSON.parse(xhr.responseText)
                    if (data.needs_target) {
                        showTargetSelection(driveName, data.candidates, 0)
                    } else if (data.ok) {
                        importingDrive = driveName
                        showProgressDialog(driveName)
                        waitingForResult = true
                        pollTimer.interval = 1500
                    }
                } catch(e) {
                    showError("Unexpected response.")
                }
            }
        }
        xhr.send(JSON.stringify({drive: driveName}))
    }

    /* Step 2b: Target selection — cycle through candidates */
    function showTargetSelection(usbDrive, candidates, index) {
        if (!candidates || candidates.length === 0) {
            showError("No internal drive with Engine Library found.")
            return
        }
        if (index >= candidates.length) {
            importInProgress = false
            pendingDrive = ""
            return
        }

        var list = ""
        for (var i = 0; i < candidates.length; i++) {
            if (i === index)
                list += "> " + candidates[i] + " (internal?)\n"
            else
                list += "   " + candidates[i] + "\n"
        }

        var msg = "Select internal hard drive:\n\n" + list

        var isLast = (index === candidates.length - 1)
        var prompt = PlanckGUI.views.prompts.post({
            title: "Select Target",
            message: msg,
            options: [
                { text: candidates[index], role: Prompt.AcceptRole },
                { text: isLast ? "Cancel" : "Next", role: Prompt.RejectRole }
            ]
        })
        prompt.finished.connect(function(role) {
            if (role === Prompt.AcceptRole)
                askRemember(usbDrive, candidates[index])
            else if (!isLast)
                showTargetSelection(usbDrive, candidates, index + 1)
            else {
                importInProgress = false
                pendingDrive = ""
            }
        })
    }

    /* Step 2c: Remember as default? */
    function askRemember(usbDrive, targetDrive) {
        var prompt = PlanckGUI.views.prompts.post({
            title: "Remember?",
            message: "Always use '" + targetDrive + "'?\n\nReset in Mod Hub.",
            options: [
                { text: "Remember", role: Prompt.AcceptRole },
                { text: "Just once", role: Prompt.RejectRole }
            ]
        })
        prompt.finished.connect(function(role) {
            selectTargetAndImport(usbDrive, targetDrive, role === Prompt.AcceptRole)
        })
    }

    /* Step 2d: Select target, then start import */
    function selectTargetAndImport(usbDrive, targetDrive, remember) {
        var xhr = new XMLHttpRequest()
        xhr.open("POST", "http://127.0.0.1:8082/api/select-target")
        xhr.setRequestHeader("Content-Type", "application/json")
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                tryStartImport(usbDrive)
            }
        }
        xhr.send(JSON.stringify({drive: targetDrive, remember: remember}))
    }

    /* Step 3: Progress dialog */
    function showProgressDialog(driveName) {
        watchdogMisses = 0
        PlanckGUI.views.prompts.post({
            id: "UsbImporting",
            title: "Importing...",
            message: "Scanning '" + driveName + "' for music...",
            options: [
                { text: "Cancel", role: Prompt.RejectRole }
            ]
        }).finished.connect(function(role) {
            if (role === Prompt.RejectRole && root.waitingForResult)
                cancelImport()
        })
    }

    function checkProgress() {
        var xhr = new XMLHttpRequest()
        xhr.open("GET", "http://127.0.0.1:8082/api/status")
        xhr.timeout = 2000
        xhr.onerror = function() { handleWatchdog() }
        xhr.ontimeout = function() { handleWatchdog() }
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                if (xhr.status !== 200) { handleWatchdog(); return }
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
                            options: [
                                { text: "Cancel", role: Prompt.RejectRole }
                            ]
                        }).finished.connect(function(role) {
                            if (role === Prompt.RejectRole && root.waitingForResult)
                                cancelImport()
                        })
                    } else if (data.done) {
                        waitingForResult = false
                        pollTimer.interval = 3000
                        PlanckGUI.views.prompts.drop("UsbImporting")
                        ackDone()

                        if (data.error && data.error.length > 0) {
                            showError(data.error)
                        } else if (data.restart_pending) {
                            showRestartDialog(data)
                        } else {
                            importInProgress = false
                            importingDrive = ""
                            pendingDrive = ""
                            var msg2 = buildResult(data)
                            PlanckGUI.views.prompts.post({
                                title: "Import Complete",
                                message: msg2,
                                options: [{ text: "OK", role: Prompt.AcceptRole }]
                            })
                        }
                    } else if (data.idle || data.new_drive) {
                        waitingForResult = false
                        pollTimer.interval = 3000
                        PlanckGUI.views.prompts.drop("UsbImporting")
                        importInProgress = false
                        importingDrive = ""
                        pendingDrive = ""
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
            pendingDrive = ""
            showError("Lost connection to import daemon.")
            watchdogMisses = 0
        }
    }

    /* Step 4: Restart dialog */
    function showRestartDialog(data) {
        var msg = buildResult(data) + "\n\nRestart EngineX to see tracks?\nPlayback will stop."
        var prompt = PlanckGUI.views.prompts.post({
            title: "Import Complete",
            message: msg,
            options: [
                { text: "Restart Now", role: Prompt.AcceptRole },
                { text: "Later", role: Prompt.RejectRole }
            ]
        })
        prompt.finished.connect(function(role) {
            importInProgress = false
            importingDrive = ""
            pendingDrive = ""
            if (role === Prompt.AcceptRole)
                restartEngine()
            else
                notifyLater()
        })
    }

    function buildResult(data) {
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
            msg += "\nTracks added to collection."
        return msg
    }

    function showError(message) {
        importInProgress = false
        importingDrive = ""
        pendingDrive = ""
        PlanckGUI.views.prompts.post({
            title: "Import Error",
            message: message,
            options: [{ text: "OK", role: Prompt.AcceptRole }]
        })
    }

    /* API calls */
    function dismissDrive(driveName) {
        var xhr = new XMLHttpRequest()
        xhr.open("POST", "http://127.0.0.1:8082/api/dismiss")
        xhr.setRequestHeader("Content-Type", "application/json")
        xhr.send(JSON.stringify({drive: driveName}))
    }

    function cancelImport() {
        var xhr = new XMLHttpRequest()
        xhr.open("POST", "http://127.0.0.1:8082/api/cancel")
        xhr.send()
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
