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
import android.view.Gravity;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.widget.EditText;
import android.widget.FrameLayout;
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

    // ── Soft IME text capture ───────────────────────────────────────────
    // Modern soft keyboards (Gboard, SwiftKey, Samsung Keyboard) commit
    // text via InputConnection.commitText() — they do NOT generate
    // hardware KeyEvents for letter keys, so dispatchKeyEvent never fires
    // and the original NativeActivity-only path silently drops every
    // character. We work around this by adding a 1×1, fully-transparent,
    // focusable EditText overlay; the IME targets that EditText and its
    // TextWatcher pushes characters into the same unicodeCharacterQueue
    // that pollUnicodeChar() drains for ImGui.
    private var imeCaptureView: EditText? = null;
    // True between showSoftInput() and hideSoftInput(). The capture view's
    // OnFocusChangeListener uses this to decide whether to re-fight when
    // NativeContentView reclaims focus mid-edit. Without it we'd thrash
    // focus back to the EditText even when no field is active.
    @Volatile private var imeKeepFocus: Boolean = false

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
        installImeCaptureView();

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
        imeKeepFocus = true
        runOnUiThread {
            val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
            val capture = imeCaptureView
            if (capture != null) {
                // NativeActivity's NativeContentView aggressively reclaims
                // focus, so we must FIGHT for it every time the IME is
                // raised: bring the EditText to the front of the z-order,
                // clear focus from whichever view currently owns it, then
                // requestFocus on the EditText. If requestFocus returns
                // false we log it — that's the smoking gun if typing
                // doesn't work, visible in `adb logcat -s PredatorRF`.
                capture.bringToFront()
                window.decorView.clearFocus()
                val gotFocus = capture.requestFocus()
                if (!gotFocus) {
                    Log.w(TAG, "imeCaptureView.requestFocus() returned false — " +
                               "IME will commit text into the wrong view; " +
                               "hasFocus=${capture.hasFocus()} " +
                               "isFocusable=${capture.isFocusable} " +
                               "windowToken=${capture.windowToken}")
                }
                // restartInput forces the IME to re-bind its InputConnection
                // to the currently-focused view. Without this, the IME may
                // keep its old binding (often to NativeContentView, which
                // accepts no text) even after our requestFocus() succeeds.
                imm.restartInput(capture)
                val shown = imm.showSoftInput(capture, InputMethodManager.SHOW_FORCED)
                Log.i(TAG, "showSoftInput: focus=$gotFocus shown=$shown " +
                           "imeBot=$imeBottomInset")
                // Post-frame reassert: NativeContentView can reclaim focus
                // on the very next layout pass after our requestFocus(),
                // which would silently rebind the IME to the wrong view
                // and drop every keystroke. Re-check on the EditText's own
                // handler one frame later and re-fight if needed.
                capture.post {
                    // Re-check imeKeepFocus: a fast show→hide transition
                    // (open modal then immediately close) can flip the
                    // flag false between scheduling and running this
                    // runnable. Without the gate we'd re-force the IME
                    // back open after the operator dismissed it.
                    if (imeKeepFocus && !capture.hasFocus()) {
                        Log.w(TAG, "imeCapture lost focus on next frame — " +
                                   "reasserting")
                        capture.bringToFront()
                        capture.requestFocus()
                        imm.restartInput(capture)
                        imm.showSoftInput(capture, InputMethodManager.SHOW_FORCED)
                    }
                }
            } else {
                imm.showSoftInput(window.decorView, InputMethodManager.SHOW_FORCED)
            }
        }
    }

    fun hideSoftInput() {
        imeKeepFocus = false
        runOnUiThread {
            val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
            val capture = imeCaptureView
            if (capture != null) {
                imm.hideSoftInputFromWindow(capture.windowToken, 0)
                capture.clearFocus()
            } else {
                imm.hideSoftInputFromWindow(window.decorView.windowToken,
                                           InputMethodManager.HIDE_IMPLICIT_ONLY)
            }
            hideSystemBars()
        }
    }

    /**
     * Install the invisible EditText that captures soft-keyboard text.
     *
     * Notes:
     *   - 1×1 pixel, alpha 0, gravity TOP|START — visually invisible.
     *   - isClickable=false so the view never intercepts touches that
     *     should reach the GLSurfaceView underneath.
     *   - TYPE_TEXT_FLAG_NO_SUGGESTIONS / NO_AUTOCORRECT prevent the IME
     *     from buffering several characters before committing them.
     *   - IME_FLAG_NO_EXTRACT_UI / NO_FULLSCREEN keep landscape Gboard
     *     from going fullscreen and covering the whole app.
     *   - The TextWatcher reads NEW characters from the [start, start+count)
     *     range and pushes each codepoint into unicodeCharacterQueue. We
     *     reset the buffer once it grows past a threshold so it doesn't
     *     leak memory across long edit sessions.
     *   - KEYCODE_DEL is bridged to ASCII 0x08 (Backspace) so the existing
     *     PollUnicodeChars()/io.AddInputCharacter() pipeline handles it.
     */
    private fun installImeCaptureView() {
        val edit = EditText(this).apply {
            setBackgroundColor(0)
            setTextColor(0)
            // alpha=0 makes some IMEs (notably Samsung Keyboard) refuse to
            // bind because the view is treated as invisible. 0.01 is below
            // human perception but counts as visible to the IME service.
            alpha = 0.01f
            isFocusable = true
            isFocusableInTouchMode = true
            isClickable = false
            isLongClickable = false
            setSingleLine(true)
            inputType = InputType.TYPE_CLASS_TEXT or
                        InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS or
                        InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
            imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN or
                         EditorInfo.IME_FLAG_NO_EXTRACT_UI
            // Defaults to TYPE_CLASS_TEXT which auto-lowercases the first
            // letter — annoying for hostnames / hex IFAC keys. Disable.
            privateImeOptions = "nm"
        }
        // Captured in beforeTextChanged so we can convert UTF-16 unit
        // deletions back into the correct number of CODE POINT backspaces.
        // Without this, deleting a single emoji (surrogate pair, before=2)
        // would emit 2× 0x08 and over-delete in ImGui.
        var pendingDeleteCodepoints = 0
        edit.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {
                // Only the leading prefix [start, start+after) is kept
                // unchanged; bytes [start+after, start+count) are removed
                // (and may then be replaced — pure deletion is after<count
                // with the new region empty). Count code points in the
                // removed range.
                pendingDeleteCodepoints = 0
                if (s == null || after >= count) return
                val delStart = start + after
                val delEnd = (start + count).coerceAtMost(s.length)
                var i = delStart
                while (i < delEnd) {
                    pendingDeleteCodepoints++
                    i += Character.charCount(Character.codePointAt(s, i))
                }
            }
            override fun afterTextChanged(s: Editable?) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {
                if (s == null) return
                // Soft-IME deletion path: many keyboards (Gboard, SwiftKey)
                // do NOT fire KEYCODE_DEL — they call deleteSurroundingText
                // on the InputConnection, which arrives here as
                // (before > 0, count == 0). Bridge to ASCII Backspace once
                // per deleted CODE POINT (computed in beforeTextChanged)
                // so ImGui's InputText shrinks by the same number of
                // logical characters the user expected — emoji included.
                if (count == 0 && before > 0) {
                    val n = if (pendingDeleteCodepoints > 0) pendingDeleteCodepoints else 1
                    repeat(n) { unicodeCharacterQueue.offer(0x08) }
                    pendingDeleteCodepoints = 0
                    return
                }
                if (count <= 0) return
                // Walk the inserted span by Unicode CODE POINT, not UTF-16
                // unit, so surrogate-pair emoji (U+1F600 etc.) and other
                // astral characters arrive intact. Character.codePointAt
                // returns the full code point at the given UTF-16 index;
                // charCount() is 2 for surrogate pairs, 1 otherwise.
                val end = (start + count).coerceAtMost(s.length)
                var i = start
                while (i < end) {
                    val cp = Character.codePointAt(s, i)
                    if (cp != 0) unicodeCharacterQueue.offer(cp)
                    i += Character.charCount(cp)
                }
                // Don't let the buffer grow without bound across an edit
                // session. Reset on the next UI tick so we don't recurse.
                if (s.length > 32) {
                    edit.post {
                        edit.removeTextChangedListener(this)
                        edit.setText("")
                        edit.addTextChangedListener(this)
                    }
                }
            }
        })
        edit.setOnKeyListener { _, keyCode, event ->
            // Hardware DEL on a NON-empty EditText also fires onTextChanged
            // with (before>0, count==0) — letting both fire would double
            // every backspace. Only synthesize 0x08 here when the buffer
            // is empty (no TextWatcher delete branch will follow), so the
            // user can still backspace ImGui's content after we've reset
            // the capture buffer.
            if (event.action == KeyEvent.ACTION_DOWN &&
                keyCode == KeyEvent.KEYCODE_DEL &&
                edit.text.isNullOrEmpty()) {
                unicodeCharacterQueue.offer(0x08)  // ASCII Backspace
            }
            false
        }
        // Sustained focus enforcement: NativeContentView can reclaim focus
        // at any later point (window state changes, popup mutations,
        // surface invalidation). One-shot show + post-frame reassert in
        // showSoftInput() doesn't catch losses that happen seconds later.
        // While the IME is supposed to be up (imeKeepFocus), every focus
        // loss re-fights for it. Listener fires on the UI thread.
        edit.onFocusChangeListener = android.view.View.OnFocusChangeListener { v, hasFocus ->
            if (!hasFocus && imeKeepFocus) {
                Log.w(TAG, "imeCapture lost focus while IME wanted — re-fighting")
                v.post {
                    // Re-check imeKeepFocus inside the runnable too: a
                    // hide() may have arrived between schedule and run.
                    // Without this gate we'd reopen the IME the operator
                    // just dismissed.
                    if (!imeKeepFocus) return@post
                    v.bringToFront()
                    v.requestFocus()
                    val imm = getSystemService(Context.INPUT_METHOD_SERVICE)
                            as InputMethodManager
                    imm.restartInput(v)
                    imm.showSoftInput(v, InputMethodManager.SHOW_FORCED)
                }
            }
        }
        // 4×4 px (not 1×1) — some keyboards' visibility heuristics treat
        // 1×1 views as invisible and refuse to bind. Still imperceptible
        // at any sane DPI.
        val params = FrameLayout.LayoutParams(4, 4).apply {
            gravity = Gravity.TOP or Gravity.START
        }
        addContentView(edit, params)
        imeCaptureView = edit

        // Defer the initial bringToFront/requestFocus until the view is
        // attached to the window — calling requestFocus before attachment
        // is a silent no-op. Post to the EditText's own handler so we run
        // after the next layout pass.
        edit.post {
            edit.bringToFront()
            val ok = edit.requestFocus()
            Log.i(TAG, "imeCaptureView installed: requestFocus=$ok " +
                       "isAttached=${edit.isAttachedToWindow} " +
                       "hasWindowFocus=${edit.hasWindowFocus()}")
        }

        // Make the FrameLayout root yield focus to descendants (us) on
        // focus-search instead of grabbing it for the NativeContentView.
        // The root is the parent of whatever addContentView appended into.
        edit.post {
            val root = edit.rootView as? android.view.ViewGroup
            root?.descendantFocusability =
                android.view.ViewGroup.FOCUS_AFTER_DESCENDANTS
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
