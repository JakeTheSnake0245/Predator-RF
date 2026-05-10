package org.sdrpp.sdrpp

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.system.Os
import android.util.Log
import com.chaquo.python.PyException
import com.chaquo.python.Python
import com.chaquo.python.android.AndroidPlatform
import org.json.JSONObject
import java.io.File

/**
 * Foreground service that starts the Predator-RF Python backend on-device
 * via Chaquopy.  Runs in the same process as MainActivity so env vars
 * exported by Python (PREDATOR_RNS_SOCK) are immediately visible to the
 * C++ native library via getenv().
 *
 * Entry point: backend.android_service.main(filesDir)
 *   - Sets HOME = filesDir
 *   - Starts FastAPI at 127.0.0.1:5259  (MapActivity polls this)
 *   - Starts RNS daemon + control socket (kujhad_rns.h connects here)
 *   - Starts TDOA coordinator + TrackManager
 */
class PredatorBackendService : Service() {

    private var backendThread: Thread? = null

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
        startForeground(NOTIFICATION_ID, buildNotification())
        startBackend()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int =
        START_STICKY   // restart automatically if the process is killed

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        backendThread?.interrupt()
        super.onDestroy()
    }

    /**
     * Bridge C++ UI config (config.json) into process env vars BEFORE the
     * Python backend boots — the backend's `cot_enabled`, `cot_dest_host`,
     * `cot_dest_port` are read from `COT_ENABLED` / `COT_DEST_HOST` /
     * `COT_DEST_PORT` exactly once at startup.  Without this bridge the
     * operator can tick "Enable TAK CoT reporting" in the C++ UI but the
     * Python CoTEmitter stays disabled (logcat: "CoTEmitter disabled
     * (cot_enabled=false)") because the two config systems never meet.
     *
     * Determinism rules (avoid stale env between launches in same process):
     *   - COT_ENABLED is ALWAYS set ("1" or "0") so a toggle-off in the UI
     *     can't be masked by a stale "1" left over from a previous run.
     *   - COT_DEST_HOST / COT_DEST_PORT are set when present and explicitly
     *     UNSET when absent, so clearing host/port in the UI takes effect.
     *
     * Read race: C++ `ConfigManager::save` overwrites config.json
     * non-atomically, so we may briefly read a truncated file mid-write.
     * Retry the parse a few times (~50ms each) before giving up.
     *
     * NOTE: Because env vars are read once at boot, toggling the checkbox
     * after launch requires an app restart to take effect on the backend
     * side.  The C++ CotReporter still picks up changes live every frame.
     */
    private fun bridgeCppConfigToEnv() {
        val configFile = File(filesDir, "config.json")

        var json: JSONObject? = null
        if (configFile.exists()) {
            for (attempt in 1..3) {
                try {
                    json = JSONObject(configFile.readText())
                    break
                } catch (e: Exception) {
                    if (attempt == 3) {
                        Log.w(TAG, "config.json unreadable after 3 attempts " +
                                "(possibly mid-write): ${e.message} — using CoT defaults (off)")
                    } else {
                        try { Thread.sleep(50) } catch (_: InterruptedException) {}
                    }
                }
            }
        } else {
            Log.i(TAG, "config.json not present yet — backend will use CoT defaults (off)")
        }

        // Always write COT_ENABLED so a UI toggle-off can't be masked by stale env.
        val enabled = json?.optBoolean("cotEnabled", false) ?: false
        Os.setenv("COT_ENABLED", if (enabled) "1" else "0", true)

        // Host: set when valid, unset otherwise (prevents stale destination).
        val host = json?.optString("cotHost", "") ?: ""
        if (host.isNotEmpty()) {
            Os.setenv("COT_DEST_HOST", host, true)
        } else {
            try { Os.unsetenv("COT_DEST_HOST") } catch (_: Exception) {}
        }

        // Port: same rule.
        val port = json?.optInt("cotPort", -1) ?: -1
        if (port > 0) {
            Os.setenv("COT_DEST_PORT", port.toString(), true)
        } else {
            try { Os.unsetenv("COT_DEST_PORT") } catch (_: Exception) {}
        }

        Log.i(TAG, "Bridged C++ → backend env: COT_ENABLED=$enabled" +
                " host=${if (host.isNotEmpty()) host else "(unset/default)"}" +
                " port=${if (port > 0) port.toString() else "(unset/default)"}")
    }

    private fun startBackend() {
        // Must run BEFORE Python.start() so the backend's dataclass
        // default_factory env reads see the operator's choice.
        bridgeCppConfigToEnv()

        if (Python.isStarted()) {
            // Warm interpreter (e.g. service auto-restart by START_STICKY in the
            // same process): backend.config was already imported, its module-level
            // singleton `config` is frozen from the prior boot, and our env edits
            // above won't be re-read.  The operator must fully restart the app
            // (force-stop) for backend-side CoT changes to take effect.
            Log.w(TAG, "Python interpreter already started — backend config is " +
                    "frozen from prior boot; CoT env changes won't apply until " +
                    "the app process is fully restarted (force-stop + relaunch).")
        } else {
            Python.start(AndroidPlatform(this))
        }
        val filesPath = filesDir.absolutePath
        backendThread = Thread({
            try {
                Log.i(TAG, "Starting Python backend at $filesPath")
                val py = Python.getInstance()
                py.getModule("backend.android_service")
                    .callAttr("main", filesPath)
            } catch (e: PyException) {
                Log.e(TAG, "Python backend exited with exception: ${e.message}")
            } catch (e: InterruptedException) {
                Log.i(TAG, "Backend thread interrupted — shutting down")
            } catch (e: Exception) {
                Log.e(TAG, "Backend thread crashed: $e")
            }
        }, "predator-backend").also {
            it.isDaemon = true
            it.start()
        }
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "Predator RF Backend",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "RNS daemon, TDOA, and signal-processing pipeline"
            }
            (getSystemService(NOTIFICATION_SERVICE) as NotificationManager)
                .createNotificationChannel(channel)
        }
    }

    private fun buildNotification(): Notification {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Notification.Builder(this, CHANNEL_ID)
                .setContentTitle("Predator RF")
                .setContentText("Backend running (RNS · TDOA · Tracks)")
                .setSmallIcon(android.R.drawable.ic_menu_compass)
                .setOngoing(true)
                .build()
        } else {
            @Suppress("DEPRECATION")
            Notification.Builder(this)
                .setContentTitle("Predator RF")
                .setContentText("Backend running (RNS · TDOA · Tracks)")
                .setSmallIcon(android.R.drawable.ic_menu_compass)
                .setOngoing(true)
                .build()
        }
    }

    companion object {
        private const val TAG = "PredatorBackend"
        private const val CHANNEL_ID = "predator_backend"
        private const val NOTIFICATION_ID = 1337
    }
}
