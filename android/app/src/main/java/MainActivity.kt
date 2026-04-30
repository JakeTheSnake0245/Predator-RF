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
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.KeyEvent;
import android.view.inputmethod.InputMethodManager;
import android.util.Log;
import android.content.res.AssetManager;

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

    // Cached IME bottom inset in pixels. Polled every frame from the
    // native side via JNI (getImeBottomInsetPx). We update it from the
    // global layout listener installed in onCreate so we don't pay an
    // androidx call per native frame.
    @Volatile private var imeBottomInsetPx: Int = 0
    @Volatile private var imeWasOpen: Boolean = false
    private var imeLayoutListener: android.view.ViewTreeObserver.OnGlobalLayoutListener? = null

    fun getImeBottomInsetPx(): Int {
        return imeBottomInsetPx
    }

    private fun installImeInsetListener() {
        val rootView = window.decorView
        val r = android.graphics.Rect()
        val listener = android.view.ViewTreeObserver.OnGlobalLayoutListener {
            // Visible-display-frame difference works on every API level
            // we care about (minSdk 28) and is robust to fullscreen +
            // adjustResize quirks on NativeActivity, which doesn't lay
            // out child Views and so doesn't get reliable WindowInsets
            // dispatches. Anything below 15 % of the screen height is
            // treated as noise (status / nav bars under immersive
            // sticky); the IME on every supported device is far taller.
            rootView.getWindowVisibleDisplayFrame(r)
            val screenH = rootView.height
            val keypadH = screenH - r.bottom
            val newInset = if (screenH > 0 && keypadH > screenH * 0.15) keypadH else 0
            if (newInset != imeBottomInsetPx) {
                imeBottomInsetPx = newInset
            }
            // Re-apply immersive flags after the IME closes so the
            // status / navigation bars stay hidden on every device,
            // even those where adjustResize fights the fullscreen
            // theme on hide.
            val nowOpen = newInset > 0
            if (imeWasOpen && !nowOpen) {
                hideSystemBars()
            }
            imeWasOpen = nowOpen
        }
        rootView.viewTreeObserver.addOnGlobalLayoutListener(listener)
        imeLayoutListener = listener
    }

    public override fun onDestroy() {
        // Detach the global-layout listener so the decor view's
        // ViewTreeObserver doesn't keep a reference to this Activity
        // across configuration / process recreation cycles.
        imeLayoutListener?.let {
            window.decorView.viewTreeObserver.removeOnGlobalLayoutListener(it)
            imeLayoutListener = null
        }
        super.onDestroy()
    }

    public override fun onCreate(savedInstanceState: Bundle?) {
        // Hide bars
        hideSystemBars();

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

        // Get permission for all USB devices
        val devList = usbManager!!.getDeviceList();
        for ((_, dev) in devList) {
            usbManager!!.requestPermission(dev, permissionIntent);
        }

        super.onCreate(savedInstanceState)
        installImeInsetListener();
        startLocationUpdates();
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

    fun showSoftInput() {
        val inputMethodManager = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager;
        inputMethodManager.showSoftInput(window.decorView, 0);
    }

    fun hideSoftInput() {
        val inputMethodManager = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager;
        inputMethodManager.hideSoftInputFromWindow(window.decorView.windowToken, 0);
        hideSystemBars();
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

    fun getDisplayHeightPx(): Int {
        return resources.displayMetrics.heightPixels;
    }

    fun getDisplayWidthPx(): Int {
        return resources.displayMetrics.widthPixels;
    }

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
