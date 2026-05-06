package org.sdrpp.sdrpp

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.util.Log
import com.chaquo.python.PyException
import com.chaquo.python.Python
import com.chaquo.python.android.AndroidPlatform

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

    private fun startBackend() {
        if (!Python.isStarted()) {
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
