// Predator RF — Android Kujhad-tab control client for the in-process
// RNS daemon (task #27).
//
// The canonical RNS implementation lives in `backend/rns/` (Python).
// On Android the daemon runs inside the embedded CPython worker that
// already hosts the FastAPI backend on the phone. This Kotlin layer
// is an in-process control surface for the Kujhad UI:
//
//   * It talks to the daemon over the same Unix socket the Linux GUI
//     uses (`backend/rns/daemon.py::ControlServer`), via Android's
//     `android.net.LocalSocket` API.
//   * No TCP / HTTP listener is opened. The control plane is NEVER
//     exposed on the network — neither to the LAN nor to other apps
//     on the device (filesystem permission 0600 on the socket).
//   * Each Kotlin method maps 1:1 to a daemon method name (status,
//     listInterfaces, addInterface, restartInterface, exportConfig …)
//     so callers see a plain function-call surface; the wire format
//     (line-delimited JSON {id,method,params}) stays internal.
//
// Outbound CoT goes through the normal CoTEmitter fan-out hook in
// `backend/output/cot_emitter.py` (the same one the Linux build uses).
// Inbound RNS-CoT is forwarded to the local ATAK app on the phone via
// its standard CoT input port (UDP 4242 by default on Android).
//
// See:
//   * backend/rns/README.md           — architecture + Android decision
//   * docs/rns_parity.md              — Linux↔Android parity matrix
//   * backend/rns/daemon.py           — control API + ControlServer

package com.predator.rf

import android.content.Context
import android.net.LocalSocket
import android.net.LocalSocketAddress
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.util.concurrent.atomic.AtomicLong

/**
 * In-process control client for `backend.rns.daemon.RNSDaemon`.
 * Communicates with the daemon over its local Unix socket; all calls
 * are synchronous and SHOULD be invoked off the UI thread.
 *
 * Socket path: `PREDATOR_RNS_SOCK` system property if set, otherwise
 * the daemon's standard non-root path
 * (`<HOME>/.local/state/predator-rns/control.sock`). On Android the
 * embedded backend exports `PREDATOR_RNS_SOCK` at startup so the UI
 * always finds the right socket.
 */
class RnsBridge(private val context: Context) {

    private val sockPath: String =
        System.getProperty("PREDATOR_RNS_SOCK")
            ?: (System.getProperty("user.home") ?: "/data/data/com.predator.rf") +
               "/.local/state/predator-rns/control.sock"

    private val nextId = AtomicLong(1)

    // ── wire plumbing ─────────────────────────────────────────────────

    /** Send one JSON-RPC-style request and return the parsed response.
     *  Throws on transport failure or daemon-reported error. */
    @Synchronized
    private fun call(method: String, params: JSONObject): JSONObject {
        val sock = LocalSocket()
        try {
            sock.connect(LocalSocketAddress(sockPath,
                LocalSocketAddress.Namespace.FILESYSTEM))
            sock.soTimeout = 30_000
            val writer = OutputStreamWriter(sock.outputStream, Charsets.UTF_8)
            val reader = BufferedReader(InputStreamReader(
                sock.inputStream, Charsets.UTF_8))
            val req = JSONObject()
                .put("id", nextId.getAndIncrement())
                .put("method", method)
                .put("params", params)
            writer.write(req.toString()); writer.write("\n"); writer.flush()
            val line = reader.readLine()
                ?: throw IllegalStateException(
                    "RNS daemon closed control socket before responding")
            val resp = JSONObject(line)
            if (!resp.optBoolean("ok", false)) {
                throw IllegalStateException(
                    "RNS daemon error: ${resp.optString("error", "unknown")}")
            }
            // Always return an object — callers wrap arrays/scalars in
            // a top-level JSONObject as needed.
            return resp
        } finally {
            try { sock.close() } catch (_: Throwable) {}
        }
    }

    private fun callObj(method: String, params: JSONObject): JSONObject =
        call(method, params).optJSONObject("result") ?: JSONObject()

    private fun callArr(method: String, params: JSONObject): JSONArray =
        call(method, params).optJSONArray("result") ?: JSONArray()

    private fun callBool(method: String, params: JSONObject): Boolean =
        call(method, params).optBoolean("result", false)

    // ── Control API (1:1 with backend/rns/daemon.py methods) ──────────

    fun status(): JSONObject =
        callObj("status", JSONObject())

    fun listInterfaces(): JSONArray =
        callArr("list_interfaces", JSONObject())

    fun getInterface(id: String): JSONObject? {
        val r = call("get_interface", JSONObject().put("iid", id))
        return r.opt("result") as? JSONObject
    }

    /** `cfgJson` is the per-interface schema documented in
     *  backend/rns/schema.py. */
    fun addInterface(cfgJson: JSONObject): JSONObject =
        callObj("add_interface", JSONObject().put("cfg", cfgJson))

    fun updateInterface(id: String, cfgJson: JSONObject): JSONObject =
        callObj("update_interface",
            JSONObject().put("iid", id).put("cfg", cfgJson))

    fun removeInterface(id: String): Boolean =
        callBool("remove_interface", JSONObject().put("iid", id))

    fun setEnabled(id: String, enabled: Boolean): JSONObject =
        callObj("set_enabled",
            JSONObject().put("iid", id).put("enabled", enabled))

    fun restartInterface(id: String): JSONObject =
        callObj("restart_interface", JSONObject().put("iid", id))

    fun restartAll(): JSONArray =
        callArr("restart_all", JSONObject())

    fun validateInterface(cfgJson: JSONObject): JSONObject =
        callObj("validate_interface", JSONObject().put("cfg", cfgJson))

    /** Returns `{"token": "prf-rns-v1.*"}`. */
    fun exportConfig(passphrase: String, includeIdentity: Boolean): JSONObject =
        callObj("export_config",
            JSONObject()
                .put("passphrase", passphrase)
                .put("include_identity", includeIdentity))

    fun importConfig(token: String, passphrase: String,
                     placeholdersJson: JSONObject): JSONObject =
        callObj("import_config",
            JSONObject()
                .put("token", token)
                .put("passphrase", passphrase)
                .put("placeholders", placeholdersJson))

    fun mintReplicationToken(newPassphrase: String,
                             includeIdentity: Boolean): JSONObject =
        callObj("mint_replication_token",
            JSONObject()
                .put("new_passphrase", newPassphrase)
                .put("include_identity", includeIdentity))

    fun getLogs(level: String, sinceMs: Long, limit: Int): JSONArray =
        callArr("get_logs",
            JSONObject()
                .put("level", level)
                .put("since_ms", sinceMs)
                .put("limit", limit))
}
