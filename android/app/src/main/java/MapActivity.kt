package org.sdrpp.sdrpp

import android.Manifest
import android.content.pm.PackageManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.webkit.JavascriptInterface
import android.webkit.WebChromeClient
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.ImageButton
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import org.json.JSONObject
import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.Executors

class MapActivity : AppCompatActivity() {
    private lateinit var mapView: WebView
    private lateinit var gpsStatus: TextView
    private var locationManager: LocationManager? = null
    private var mapReady = false
    private var lastLocation: Location? = null
    private var followMode = true

    // Track polling — polls /api/v1/android-pull every 5 s and pushes
    // the track list to the map's updateTracks() bridge method.
    private val pollExecutor = Executors.newSingleThreadExecutor()
    private val mainHandler  = Handler(Looper.getMainLooper())
    private var pollCursor   = 0L
    private var polling      = false
    private val pollRunnable = object : Runnable {
        override fun run() {
            if (!polling) return
            pollExecutor.execute {
                fetchAndPushTracks()
                pushPeerMarkers()
                pushEventMarkers()
                pushFleetLobs()
                pushAouGrid()
                mainHandler.postDelayed(this, POLL_INTERVAL_MS)
            }
        }
    }

    companion object {
        // Read from shared prefs / BuildConfig; fall back to localhost.
        private const val BACKEND_BASE = "http://127.0.0.1:5259"
        private const val POLL_INTERVAL_MS = 5_000L
        // Kraken tune lifecycle feedback wants ~1 s latency, much faster
        // than the 5 s track poll, so it runs on its own runnable.
        private const val KRAKEN_STATUS_INTERVAL_MS = 1_000L
    }

    // Kraken tune status pusher — forwards MainActivity.krakenTuneStatusJson
    // (written by native via JNI) into the WebView every second while the
    // map is open, but only when the JSON actually changed.
    private var lastKrakenStatusPushed = ""
    private val krakenStatusRunnable = object : Runnable {
        override fun run() {
            if (!polling) return
            val statusJson = MainActivity.krakenTuneStatusJson
            if (statusJson != lastKrakenStatusPushed && mapReady) {
                lastKrakenStatusPushed = statusJson
                // JSONObject.quote() → fully-escaped JS string literal;
                // never inline raw JSON into evaluateJavascript.
                val jsLiteral = JSONObject.quote(statusJson)
                mapView.evaluateJavascript(
                    "window.PredatorRFMap && window.PredatorRFMap.updateKrakenStatus($jsLiteral);",
                    null
                )
            }
            mainHandler.postDelayed(this, KRAKEN_STATUS_INTERVAL_MS)
        }
    }

    // Exposed to the map WebView as window.PredatorNative. The map popup's
    // "Tune Kraken" button calls tuneKraken(freqHz); the native render loop
    // drains the pending value through MainActivity.pollKrakenTuneRequest().
    inner class PredatorNativeBridge {
        @JavascriptInterface
        fun tuneKraken(freqHz: Double) {
            if (freqHz.isFinite() && freqHz > 0.0) {
                MainActivity.pendingKrakenTuneHz = freqHz
            }
        }

        @JavascriptInterface
        fun krakenAvailable(): Boolean {
            // Cheap availability probe for the popup: native pushes
            // {"available":true,...} once a kraken module registers.
            return MainActivity.krakenTuneStatusJson.contains("\"available\":true")
        }
    }

    private val locationListener = object : LocationListener {
        override fun onLocationChanged(location: Location) {
            lastLocation = location
            gpsStatus.text = "GPS ${"%.6f".format(location.latitude)}, ${"%.6f".format(location.longitude)}  +/-${location.accuracy.toInt()}m"
            if (mapReady) {
                pushLocationToMap(location, followMode)
            }
        }

        override fun onStatusChanged(provider: String?, status: Int, extras: Bundle?) {}

        override fun onProviderEnabled(provider: String) {}

        override fun onProviderDisabled(provider: String) {}
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_map)

        mapView = findViewById(R.id.map_webview)
        gpsStatus = findViewById(R.id.map_gps_status)
        val closeButton = findViewById<ImageButton>(R.id.map_close_button)
        val recenterButton = findViewById<ImageButton>(R.id.map_recenter_button)
        val followButton = findViewById<ImageButton>(R.id.map_follow_button)

        closeButton.setOnClickListener { finish() }
        recenterButton.setOnClickListener {
            followMode = true
            followButton.alpha = 1.0f
            lastLocation?.let { pushLocationToMap(it, true) }
        }
        followButton.setOnClickListener {
            followMode = !followMode
            followButton.alpha = if (followMode) 1.0f else 0.55f
            lastLocation?.let { pushLocationToMap(it, followMode) }
        }

        mapView.settings.javaScriptEnabled = true
        mapView.settings.domStorageEnabled = true
        mapView.settings.builtInZoomControls = false
        mapView.settings.displayZoomControls = false
        mapView.webChromeClient = WebChromeClient()
        // Kraken DF tasking bridge: map popup → native render loop.
        mapView.addJavascriptInterface(PredatorNativeBridge(), "PredatorNative")
        mapView.webViewClient = object : WebViewClient() {
            override fun onPageFinished(view: WebView?, url: String?) {
                mapReady = true
                lastLocation?.let { pushLocationToMap(it, followMode) }
                startTrackPolling()
            }
        }
        mapView.loadUrl("file:///android_asset/res/maps/index.html")

        locationManager = getSystemService(LOCATION_SERVICE) as LocationManager
        ensureLocationPermission()
    }

    override fun onResume() {
        super.onResume()
        startLocationUpdates()
        if (mapReady) startTrackPolling()
    }

    override fun onPause() {
        super.onPause()
        stopLocationUpdates()
        stopTrackPolling()
    }

    override fun onDestroy() {
        stopLocationUpdates()
        stopTrackPolling()
        pollExecutor.shutdownNow()
        mapView.destroy()
        super.onDestroy()
    }

    private fun ensureLocationPermission() {
        val fine = ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION)
        val coarse = ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_COARSE_LOCATION)
        if (fine == PackageManager.PERMISSION_GRANTED || coarse == PackageManager.PERMISSION_GRANTED) {
            startLocationUpdates()
            return
        }
        ActivityCompat.requestPermissions(
            this,
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION),
            2
        )
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == 2 && grantResults.any { it == PackageManager.PERMISSION_GRANTED }) {
            startLocationUpdates()
        }
    }

    private fun startLocationUpdates() {
        val fine = ActivityCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION)
        val coarse = ActivityCompat.checkSelfPermission(this, Manifest.permission.ACCESS_COARSE_LOCATION)
        if (fine != PackageManager.PERMISSION_GRANTED && coarse != PackageManager.PERMISSION_GRANTED) {
            gpsStatus.text = "GPS permission required"
            return
        }

        val mgr = locationManager ?: return
        gpsStatus.text = "Waiting for phone GPS"

        if (mgr.isProviderEnabled(LocationManager.GPS_PROVIDER)) {
            mgr.requestLocationUpdates(LocationManager.GPS_PROVIDER, 1000L, 2.0f, locationListener)
            mgr.getLastKnownLocation(LocationManager.GPS_PROVIDER)?.let {
                lastLocation = it
                gpsStatus.text = "GPS ${"%.6f".format(it.latitude)}, ${"%.6f".format(it.longitude)}  +/-${it.accuracy.toInt()}m"
                if (mapReady) {
                    pushLocationToMap(it, followMode)
                }
            }
        }

        if (mgr.isProviderEnabled(LocationManager.NETWORK_PROVIDER)) {
            mgr.requestLocationUpdates(LocationManager.NETWORK_PROVIDER, 2000L, 5.0f, locationListener)
            if (lastLocation == null) {
                mgr.getLastKnownLocation(LocationManager.NETWORK_PROVIDER)?.let {
                    lastLocation = it
                    gpsStatus.text = "GPS ${"%.6f".format(it.latitude)}, ${"%.6f".format(it.longitude)}  +/-${it.accuracy.toInt()}m"
                    if (mapReady) {
                        pushLocationToMap(it, followMode)
                    }
                }
            }
        }
    }

    private fun stopLocationUpdates() {
        locationManager?.removeUpdates(locationListener)
    }

    private fun pushLocationToMap(location: Location, follow: Boolean) {
        val js = "window.PredatorRFMap && window.PredatorRFMap.updatePosition(${location.latitude}, ${location.longitude}, ${location.accuracy}, ${if (follow) "true" else "false"});"
        mapView.evaluateJavascript(js, null)
    }

    private fun startTrackPolling() {
        if (polling) return
        polling = true
        lastKrakenStatusPushed = ""   // force a status re-push on reopen
        mainHandler.post(pollRunnable)
        mainHandler.post(krakenStatusRunnable)
    }

    private fun stopTrackPolling() {
        polling = false
        mainHandler.removeCallbacks(pollRunnable)
        mainHandler.removeCallbacks(krakenStatusRunnable)
    }

    private fun fetchAndPushTracks() {
        try {
            val url = URL("$BACKEND_BASE/api/v1/android-pull?since_ns=$pollCursor")
            val conn = url.openConnection() as HttpURLConnection
            conn.connectTimeout = 3000
            conn.readTimeout    = 4000
            conn.requestMethod  = "GET"
            val code = conn.responseCode
            if (code != 200) return
            val body = BufferedReader(InputStreamReader(conn.inputStream)).use { it.readText() }
            conn.disconnect()
            val root   = JSONObject(body)
            pollCursor = root.optLong("cursor", pollCursor)
            val tracks = root.optJSONArray("tracks") ?: return
            // JSONObject.quote() yields a fully-escaped double-quoted JS
            // string literal (handles quotes AND backslashes), so no
            // payload in track names can break out of the literal.
            val jsLiteral = JSONObject.quote(tracks.toString())
            mainHandler.post {
                if (mapReady) {
                    mapView.evaluateJavascript(
                        "window.PredatorRFMap && window.PredatorRFMap.updateTracks($jsLiteral);",
                        null
                    )
                }
            }
        } catch (_: Exception) {
            // Network error — silently retry on next interval.
        }
    }

    // Kujhad fleet peers come straight from the native layer (no backend
    // required): MainActivity.peerMarkersJson is updated by C++ via JNI
    // whenever controller peer snapshots change. Push it to the WebView
    // peer-marker layer on every poll tick.
    private fun pushPeerMarkers() {
        // JSONObject.quote() produces a fully-escaped double-quoted JS
        // string literal — safe against quotes, backslashes, and any
        // injection payloads riding in peer-controlled names.
        val jsLiteral = JSONObject.quote(MainActivity.peerMarkersJson)
        mainHandler.post {
            if (mapReady) {
                mapView.evaluateJavascript(
                    "window.PredatorRFMap && window.PredatorRFMap.updatePeers($jsLiteral);",
                    null
                )
            }
        }
    }

    // Heard-signal event markers come from the native layer:
    // MainActivity.eventMarkersJson is updated by C++ via JNI
    // (backend::setEventMarkers). Push it to the WebView event-marker layer
    // on every poll tick, using JSONObject.quote() so no label / source
    // string can break out of the JS literal.
    private fun pushEventMarkers() {
        val jsLiteral = JSONObject.quote(MainActivity.eventMarkersJson)
        mainHandler.post {
            if (mapReady) {
                mapView.evaluateJavascript(
                    "window.PredatorRFMap && window.PredatorRFMap.updateEventMarkers($jsLiteral);",
                    null
                )
            }
        }
    }

    // Fleet-wide Kraken LOB bearings come from the native layer:
    // MainActivity.fleetLobsJson is updated by C++ via JNI
    // (backend::setFleetLobs). Push it to the WebView LOB-wedge layer on
    // every poll tick, using JSONObject.quote() so no source-device string
    // can break out of the JS literal.
    private fun pushFleetLobs() {
        val jsLiteral = JSONObject.quote(MainActivity.fleetLobsJson)
        mainHandler.post {
            if (mapReady) {
                mapView.evaluateJavascript(
                    "window.PredatorRFMap && window.PredatorRFMap.updateFleetLobs($jsLiteral);",
                    null
                )
            }
        }
    }

    // Transmitter AOU probability grids (per signal), computed natively in
    // backgroundMapTick() and stashed in MainActivity.aouGridJson via JNI.
    // Same quoting rule as pushFleetLobs — labels ride on RF-derived data.
    private fun pushAouGrid() {
        val jsLiteral = JSONObject.quote(MainActivity.aouGridJson)
        mainHandler.post {
            if (mapReady) {
                mapView.evaluateJavascript(
                    "window.PredatorRFMap && window.PredatorRFMap.updateAouGrid($jsLiteral);",
                    null
                )
            }
        }
    }
}
