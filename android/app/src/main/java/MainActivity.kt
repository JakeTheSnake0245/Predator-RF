package org.sdrpp.sdrpp;

import android.app.NativeActivity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.DialogInterface;
import android.content.pm.PackageManager;
import android.hardware.usb.*;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.Manifest;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.PowerManager;
import android.provider.OpenableColumns;
import android.view.View;
import android.view.KeyEvent;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.view.inputmethod.InputMethodManager;
import android.util.Log;
import android.content.res.AssetManager;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

import androidx.core.app.ActivityCompat;

import androidx.core.content.PermissionChecker;

import java.util.concurrent.LinkedBlockingQueue;
import java.io.*;

private const val ACTION_USB_PERMISSION = "org.sdrpp.sdrpp.USB_PERMISSION";

private val usbReceiver = object : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (ACTION_USB_PERMISSION == intent.action) {
            synchronized(this) {
                var _this = context as MainActivity;
                _this.SDR_device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
                if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                    _this.SDR_conn = _this.usbManager!!.openDevice(_this.SDR_device);
                    
                    // Save SDR info
                    _this.SDR_VID = _this.SDR_device!!.getVendorId();
                    _this.SDR_PID = _this.SDR_device!!.getProductId()
                    _this.SDR_FD = _this.SDR_conn!!.getFileDescriptor();
                }
                
                // Whatever the hell this does
                context.unregisterReceiver(this);

                // Hide again the system bars
                _this.hideSystemBars();
            }
        }
    }
}

class MainActivity : NativeActivity() {
    private val TAG : String = "Predator RF";
    public var usbManager : UsbManager? = null;
    public var SDR_device : UsbDevice? = null;
    public var SDR_conn : UsbDeviceConnection? = null;
    public var SDR_VID : Int = -1;
    public var SDR_PID : Int = -1;
    public var SDR_FD : Int = -1;
    public var gpsLat : Double = 0.0;
    public var gpsLon : Double = 0.0;
    public var gpsAccuracyMeters : Float = 0.0f;
    public var gpsHasFix : Boolean = false;
    public var locationManager : LocationManager? = null;

    // ── Window insets (notch / nav-bar safe area) and IME inset ─────────
    // Updated by setOnApplyWindowInsetsListener; published to native via
    // getSafeArea*() / getImeBottomInset(). All values in raw screen pixels.
    @Volatile public var safeTop    : Int = 0;
    @Volatile public var safeBottom : Int = 0;
    @Volatile public var safeLeft   : Int = 0;
    @Volatile public var safeRight  : Int = 0;
    @Volatile public var imeBottomInset : Int = 0;
    // For pre-API-30 (Android < 11) IME-height inference: we don't get a
    // dedicated ime-inset, so we capture the no-keyboard bottom inset as
    // a baseline and treat any growth as the keyboard.
    private var legacyBaselineBottom : Int = -1;

    // ── Thermal status (Android Q+) ─────────────────────────────────────
    // 0 = NONE, 1 = LIGHT, 2 = MODERATE, 3 = SEVERE, 4 = CRITICAL,
    // 5 = EMERGENCY, 6 = SHUTDOWN. Native side throttles scan rate /
    // FFT depth based on this so the phone doesn't melt during a sweep.
    @Volatile public var thermalStatus : Int = 0;
    private var powerManager : PowerManager? = null;
    private var thermalListener : PowerManager.OnThermalStatusChangedListener? = null;

    private val locationListener = object : LocationListener {
        override fun onLocationChanged(location: Location) {
            gpsLat = location.latitude;
            gpsLon = location.longitude;
            gpsAccuracyMeters = location.accuracy;
            gpsHasFix = true;
        }

        override fun onStatusChanged(provider: String?, status: Int, extras: Bundle?) {}

        override fun onProviderEnabled(provider: String) {}

        override fun onProviderDisabled(provider: String) {}
    }

    fun checkAndAsk(permission: String) {
        if (PermissionChecker.checkSelfPermission(this, permission) != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, arrayOf(permission), 1);
        }
    }

    fun requestMissingPermissions(vararg permissions: String) {
        val missing = permissions.filter {
            PermissionChecker.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }.toTypedArray()

        if (missing.isNotEmpty()) {
            ActivityCompat.requestPermissions(this, missing, 1)
        }
    }

    public fun hideSystemBars() {
        val decorView = getWindow().getDecorView();
        val uiOptions = View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY;
        decorView.setSystemUiVisibility(uiOptions);
    }

    /**
     * Render to the edges (under camera cutouts / notch) in landscape.
     * Combined with the safe-area inset publication below, this lets the
     * waterfall use the full width while menus still sit clear of the
     * cutout. Layout-cutout API is API 28+; minSdk is 28 so always present.
     */
    private fun configureCutoutMode() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            val lp = window.attributes
            lp.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            window.attributes = lp
        }
    }

    /**
     * Subscribe to window insets so the C++ side can pad the main window
     * around the system bars / notch and shrink it when the soft keyboard
     * comes up. The inset listener runs on the UI thread; the values are
     * @Volatile so the render thread can read them lock-free.
     */
    private fun installInsetListener() {
        window.decorView.setOnApplyWindowInsetsListener { v, insets ->
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                // Android 11+: dedicated ime() and systemBars() insets.
                val sysMask = WindowInsets.Type.systemBars() or
                              WindowInsets.Type.displayCutout()
                val sys = insets.getInsets(sysMask)
                val ime = insets.getInsets(WindowInsets.Type.ime())
                safeTop    = sys.top
                safeBottom = sys.bottom
                safeLeft   = sys.left
                safeRight  = sys.right
                imeBottomInset = ime.bottom
            } else {
                // Android 9–10: only the combined systemWindowInset* exists.
                // Capture the first-seen bottom inset as our "no keyboard"
                // baseline; any growth past that is the IME height.
                @Suppress("DEPRECATION")
                run {
                    val curBottom = insets.systemWindowInsetBottom
                    if (legacyBaselineBottom < 0) legacyBaselineBottom = curBottom
                    safeTop    = insets.systemWindowInsetTop
                    safeBottom = legacyBaselineBottom
                    safeLeft   = insets.systemWindowInsetLeft
                    safeRight  = insets.systemWindowInsetRight
                    imeBottomInset = (curBottom - legacyBaselineBottom).coerceAtLeast(0)
                }
            }
            v.onApplyWindowInsets(insets)
        }
    }

    /**
     * Subscribe to thermal status. On API 29+ Android lets us read the
     * SoC's thermal state directly so we can back off scan rate before
     * the kernel starts CPU-throttling us silently.
     */
    private fun installThermalListener() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return
        powerManager = getSystemService(Context.POWER_SERVICE) as PowerManager
        thermalStatus = powerManager!!.currentThermalStatus
        thermalListener = PowerManager.OnThermalStatusChangedListener { status ->
            thermalStatus = status
            if (status >= PowerManager.THERMAL_STATUS_SEVERE) {
                Log.w(TAG, "Thermal status SEVERE+ ($status) — native side should throttle")
            }
        }
        powerManager!!.addThermalStatusListener(thermalListener!!)
    }

    /**
     * Request USB permission for one device. Same flow as the cold-start
     * loop in onCreate, factored out so onNewIntent can reuse it.
     */
    private fun requestUsbPermissionFor(dev: UsbDevice) {
        val mgr = usbManager ?: return
        if (mgr.hasPermission(dev)) {
            // Already granted — open immediately.
            SDR_device = dev
            SDR_conn = mgr.openDevice(dev)
            if (SDR_conn != null) {
                SDR_VID = dev.vendorId
                SDR_PID = dev.productId
                SDR_FD  = SDR_conn!!.fileDescriptor
            }
            return
        }
        val pi = PendingIntent.getBroadcast(this, 0,
            Intent(ACTION_USB_PERMISSION).setPackage(packageName),
            PendingIntent.FLAG_MUTABLE)
        val filter = IntentFilter(ACTION_USB_PERMISSION)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(usbReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            registerReceiver(usbReceiver, filter)
        }
        mgr.requestPermission(dev, pi)
    }

    public override fun onCreate(savedInstanceState: Bundle?) {
        // Hide bars
        hideSystemBars();

        // Keep the screen on while the activity is foreground. Auto-released
        // when the window stops, so no explicit wakelock acquire/release.
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        // Render under camera cutout / notch in landscape.
        configureCutoutMode();

        // Ask for required permissions, without these the app cannot run.
        requestMissingPermissions(
            Manifest.permission.WRITE_EXTERNAL_STORAGE,
            Manifest.permission.READ_EXTERNAL_STORAGE,
            Manifest.permission.ACCESS_FINE_LOCATION,
            Manifest.permission.ACCESS_COARSE_LOCATION
        );

        // TODO: Have the main code wait until these two permissions are available

        // Register events
        usbManager = getSystemService(Context.USB_SERVICE) as UsbManager;
        locationManager = getSystemService(Context.LOCATION_SERVICE) as LocationManager;
        val permissionIntent = PendingIntent.getBroadcast(this, 0, Intent(ACTION_USB_PERMISSION).setPackage(packageName), PendingIntent.FLAG_MUTABLE)
        val filter = IntentFilter(ACTION_USB_PERMISSION)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(usbReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        }
        else {
            registerReceiver(usbReceiver, filter)
        }

        // Get permission for all USB devices (cold-start enumeration).
        val devList = usbManager!!.getDeviceList();
        for ((_, dev) in devList) {
            usbManager!!.requestPermission(dev, permissionIntent);
        }

        // Start the on-device Python backend (RNS + TDOA + HTTP API) before
        // the native SDR++ loop so the daemon socket is ready when the C++
        // RNS panel first tries to connect.
        val backendIntent = Intent(this, PredatorBackendService::class.java)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(backendIntent)
        } else {
            startService(backendIntent)
        }

        super.onCreate(savedInstanceState)

        // Install inset + thermal listeners after super.onCreate so the
        // window/decor view exists.
        installInsetListener();
        installThermalListener();

        startLocationUpdates();

        // Cold-launch via USB_DEVICE_ATTACHED — Android passes the device
        // in the launch intent. Treat it the same as a hot-plug.
        handleUsbAttachIntent(intent)
    }

    public override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleUsbAttachIntent(intent)
    }

    private fun handleUsbAttachIntent(intent: Intent?) {
        if (intent == null) return
        if (intent.action != UsbManager.ACTION_USB_DEVICE_ATTACHED) return
        val dev: UsbDevice? = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
        if (dev != null) {
            Log.i(TAG, "USB attached: vid=0x${"%04x".format(dev.vendorId)} pid=0x${"%04x".format(dev.productId)}")
            requestUsbPermissionFor(dev)
        }
    }

    public override fun onResume() {
        // Hide bars again
        hideSystemBars();
        startLocationUpdates();
        super.onResume();
    }

    public override fun onPause() {
        stopLocationUpdates();
        super.onPause();
    }

    public override fun onDestroy() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            thermalListener?.let { powerManager?.removeThermalStatusListener(it) }
        }
        super.onDestroy();
    }

    // Show / hide the soft keyboard. Called from the native (game) thread
    // via JNI when ImGui's io.WantTextInput rises/falls. Two non-obvious
    // requirements made the previous implementation a silent no-op:
    //
    //   1. InputMethodManager calls MUST run on the UI thread. Calling
    //      from any other thread fails silently — no exception, no log,
    //      no keyboard. This is why edit boxes appeared "dead": the
    //      InputText widget was activating correctly and io.WantTextInput
    //      was rising correctly, but the IME request was being dropped
    //      because it was issued from the native thread.
    //
    //   2. NativeActivity's decorView is NOT focusable in touch mode, so
    //      InputMethodManager.showSoftInput(view, 0) returns false because
    //      the target view can't accept input focus. The SHOW_FORCED flag
    //      bypasses the focus check and forces the IME to appear. Pair it
    //      with HIDE_IMPLICIT_ONLY on hide so the IME stays up even if
    //      another non-focusable view briefly gets focus.
    fun showSoftInput() {
        runOnUiThread {
            val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
            imm.showSoftInput(window.decorView, InputMethodManager.SHOW_FORCED)
        }
    }

    fun hideSoftInput() {
        runOnUiThread {
            val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
            imm.hideSoftInputFromWindow(window.decorView.windowToken,
                                       InputMethodManager.HIDE_IMPLICIT_ONLY)
            hideSystemBars()
        }
    }

    // Queue for the Unicode characters to be polled from native code (via pollUnicodeChar())
    private var unicodeCharacterQueue: LinkedBlockingQueue<Int> = LinkedBlockingQueue()

    // We assume dispatchKeyEvent() of the NativeActivity is actually called for every
    // KeyEvent and not consumed by any View before it reaches here
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.action == KeyEvent.ACTION_DOWN) {
            unicodeCharacterQueue.offer(event.getUnicodeChar(event.metaState))
        }
        return super.dispatchKeyEvent(event)
    }

    fun pollUnicodeChar(): Int {
        return unicodeCharacterQueue.poll() ?: 0
    }

    fun openMapView() {
        runOnUiThread {
            startActivity(Intent(this, MapActivity::class.java))
        }
    }

    // ── Storage Access Framework bridge ─────────────────────────────────
    // Replaces the desktop pfd (portable_file_dialogs) calls that silently
    // failed on Android because pfd has no Android backend. Each method
    // here BLOCKS the calling (native worker) thread until the user picks
    // or cancels — never call from the UI thread or from the main render
    // loop, only from a dedicated worker. The actual SAF intent dispatch
    // is marshalled to the UI thread internally, so we don't deadlock.
    //
    // Result conventions:
    //   safPickFileForRead  → empty string on cancel; else local cache
    //                          path containing a copy of the picked file
    //                          (so existing std::ifstream code Just Works)
    //   safPickFolder       → empty string on cancel; else the SAF tree
    //                          URI as a string (NOT a filesystem path —
    //                          callers that fopen(path) will fail; this is
    //                          intentional and surfaced upstream)
    //   safSaveFile         → false on cancel/error; true on success.
    //                          On success the contents of sourceCachePath
    //                          have been copied to the user-chosen URI.
    private val SAF_RC_PICK_FILE   = 0x5AF01
    private val SAF_RC_PICK_FOLDER = 0x5AF02
    private val SAF_RC_SAVE_FILE   = 0x5AF03

    private var safLatch: CountDownLatch? = null
    private val safResult = AtomicReference<String>("")
    // Source cache path for the in-flight save operation. Captured when
    // the caller invokes safSaveFile, read in onActivityResult.
    @Volatile private var safSaveSource: String = ""

    fun safPickFileForRead(mimeFilter: String): String {
        val latch = CountDownLatch(1)
        safLatch = latch
        safResult.set("")
        runOnUiThread {
            try {
                val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                    addCategory(Intent.CATEGORY_OPENABLE)
                    type = if (mimeFilter.isEmpty()) "*/*" else mimeFilter
                }
                startActivityForResult(intent, SAF_RC_PICK_FILE)
            } catch (e: Exception) {
                Log.e(TAG, "safPickFileForRead launch failed", e)
                safResult.set("")
                latch.countDown()
            }
        }
        latch.await()
        return safResult.get()
    }

    fun safPickFolder(): String {
        val latch = CountDownLatch(1)
        safLatch = latch
        safResult.set("")
        runOnUiThread {
            try {
                val intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
                startActivityForResult(intent, SAF_RC_PICK_FOLDER)
            } catch (e: Exception) {
                Log.e(TAG, "safPickFolder launch failed", e)
                safResult.set("")
                latch.countDown()
            }
        }
        latch.await()
        return safResult.get()
    }

    fun safSaveFile(suggestedName: String, sourceCachePath: String): Boolean {
        val latch = CountDownLatch(1)
        safLatch = latch
        safResult.set("")
        safSaveSource = sourceCachePath
        runOnUiThread {
            try {
                val intent = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
                    addCategory(Intent.CATEGORY_OPENABLE)
                    type = "application/octet-stream"
                    putExtra(Intent.EXTRA_TITLE, suggestedName)
                }
                startActivityForResult(intent, SAF_RC_SAVE_FILE)
            } catch (e: Exception) {
                Log.e(TAG, "safSaveFile launch failed", e)
                safResult.set("")
                latch.countDown()
            }
        }
        latch.await()
        val ok = safResult.get() == "OK"
        safSaveSource = ""
        return ok
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        when (requestCode) {
            SAF_RC_PICK_FILE -> {
                val uri = if (resultCode == RESULT_OK) data?.data else null
                safResult.set(if (uri != null) copyUriToCache(uri) else "")
                safLatch?.countDown()
            }
            SAF_RC_PICK_FOLDER -> {
                val uri = if (resultCode == RESULT_OK) data?.data else null
                if (uri != null) {
                    try {
                        contentResolver.takePersistableUriPermission(uri,
                            Intent.FLAG_GRANT_READ_URI_PERMISSION or
                            Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
                    } catch (e: Exception) {
                        Log.w(TAG, "takePersistableUriPermission failed", e)
                    }
                }
                safResult.set(uri?.toString() ?: "")
                safLatch?.countDown()
            }
            SAF_RC_SAVE_FILE -> {
                val uri = if (resultCode == RESULT_OK) data?.data else null
                val src = safSaveSource
                var ok = false
                if (uri != null && src.isNotEmpty()) {
                    try {
                        contentResolver.openOutputStream(uri)?.use { out ->
                            FileInputStream(src).use { it.copyTo(out) }
                        }
                        ok = true
                    } catch (e: Exception) {
                        Log.e(TAG, "safSaveFile copy failed", e)
                    }
                }
                safResult.set(if (ok) "OK" else "")
                safLatch?.countDown()
            }
            else -> { /* not ours */ }
        }
        // The IME / system bars may have hidden during the picker; restore.
        hideSystemBars()
    }

    private fun copyUriToCache(uri: Uri): String {
        return try {
            val name = queryDisplayName(uri) ?: "saf_${System.currentTimeMillis()}.bin"
            val outDir = File(cacheDir, "saf_picked")
            outDir.mkdirs()
            val out = File(outDir, name)
            contentResolver.openInputStream(uri)?.use { input ->
                FileOutputStream(out).use { input.copyTo(it) }
            } ?: return ""
            out.absolutePath
        } catch (e: Exception) {
            Log.e(TAG, "copyUriToCache failed for $uri", e)
            ""
        }
    }

    private fun queryDisplayName(uri: Uri): String? {
        return try {
            contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME),
                                  null, null, null)?.use { c ->
                if (c.moveToFirst()) c.getString(0) else null
            }
        } catch (e: Exception) { null }
    }

    fun getGpsLatitude(): Double {
        return gpsLat;
    }

    fun getGpsLongitude(): Double {
        return gpsLon;
    }

    fun getGpsAccuracy(): Float {
        return gpsAccuracyMeters;
    }

    fun hasGpsFix(): Boolean {
        return gpsHasFix;
    }

    fun getDisplayDensity(): Float {
        return resources.displayMetrics.density;
    }

    // ── JNI getters: window insets ──────────────────────────────────────
    // NOTE: getImeBottomInset() and getThermalStatus() are NOT declared
    // here — Kotlin auto-generates those from the public `var` properties
    // above, and declaring them explicitly causes a JVM signature clash
    // (Platform declaration clash on getImeBottomInset()I). The native
    // side still finds them via the auto-generated getters, so the JNI
    // bridge in backend.cpp is unaffected.
    //
    // The safe-area getters DO need explicit functions because the
    // properties are named `safeTop`/`safeBottom`/etc, not `safeAreaTop`,
    // so Kotlin would otherwise expose them as getSafeTop() — which
    // wouldn't match the C++ side's GetMethodID lookup of getSafeAreaTop.
    fun getSafeAreaTop():    Int = safeTop
    fun getSafeAreaBottom(): Int = safeBottom
    fun getSafeAreaLeft():   Int = safeLeft
    fun getSafeAreaRight():  Int = safeRight

    fun startLocationUpdates() {
        val fine = PermissionChecker.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION);
        val coarse = PermissionChecker.checkSelfPermission(this, Manifest.permission.ACCESS_COARSE_LOCATION);
        if (fine != PackageManager.PERMISSION_GRANTED && coarse != PackageManager.PERMISSION_GRANTED) {
            gpsHasFix = false;
            return;
        }

        val manager = locationManager ?: return;

        if (manager.isProviderEnabled(LocationManager.GPS_PROVIDER)) {
            manager.requestLocationUpdates(LocationManager.GPS_PROVIDER, 1000L, 2.0f, locationListener);
            val last = manager.getLastKnownLocation(LocationManager.GPS_PROVIDER);
            if (last != null) {
                gpsLat = last.latitude;
                gpsLon = last.longitude;
                gpsAccuracyMeters = last.accuracy;
                gpsHasFix = true;
            }
        }

        if (manager.isProviderEnabled(LocationManager.NETWORK_PROVIDER)) {
            manager.requestLocationUpdates(LocationManager.NETWORK_PROVIDER, 2000L, 5.0f, locationListener);
            if (!gpsHasFix) {
                val last = manager.getLastKnownLocation(LocationManager.NETWORK_PROVIDER);
                if (last != null) {
                    gpsLat = last.latitude;
                    gpsLon = last.longitude;
                    gpsAccuracyMeters = last.accuracy;
                    gpsHasFix = true;
                }
            }
        }
    }

    fun stopLocationUpdates() {
        locationManager?.removeUpdates(locationListener);
    }

    public fun createIfDoesntExist(path: String) {
        // This is a directory, create it in the filesystem
        var folder = File(path);
        var success = true;
        if (!folder.exists()) {
            success = folder.mkdirs();
        }
        if (!success) {
            Log.e(TAG, "Could not create folder with path " + path);
        }
    }

    public fun extractDir(aman: AssetManager, local: String, rsrc: String): Int {
        val flist = aman.list(rsrc) ?: return 0;
        var ecount = 0;
        for (fp in flist) {
            val lpath = local + "/" + fp;
            val rpath = rsrc + "/" + fp;

            Log.w(TAG, "Extracting '" + rpath + "' to '" + lpath + "'");

            // Create local path if non-existent
            createIfDoesntExist(local);
            
            // Create if directory
            val ext = extractDir(aman, lpath, rpath);

            // Extract if file
            if (ext == 0) {
                // This is a file, extract it
                val _os = FileOutputStream(lpath);
                val _is = aman.open(rpath);
                val ilen = _is.available();
                var fbuf = ByteArray(ilen);
                _is.read(fbuf, 0, ilen);
                _os.write(fbuf);
                _os.close();
                _is.close();
            }

            ecount++;
        }
        return ecount;
    }

    public fun getAppDir(): String {
        val fdir = getFilesDir().getAbsolutePath();

        // Extract all resources to the app directory
        val aman = getAssets();
        extractDir(aman, fdir + "/res", "res");
        createIfDoesntExist(fdir + "/modules");
        createIfDoesntExist(fdir + "/maps");
        createIfDoesntExist(fdir + "/df");

        return fdir;
    }
}
