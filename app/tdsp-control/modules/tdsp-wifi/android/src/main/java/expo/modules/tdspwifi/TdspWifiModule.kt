package expo.modules.tdspwifi

import android.annotation.TargetApi
import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiManager
import android.net.wifi.WifiNetworkSpecifier
import android.os.Build
import android.os.Handler
import android.os.Looper
import expo.modules.kotlin.Promise
import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition

// Joins a Wi-Fi network for THIS APP ONLY, using Android 10+'s WifiNetworkSpecifier:
//  * the system asks the user once ("Connect to T-DSP?"); no password typing
//  * the network is requested WITHOUT internet capability, so the phone keeps its normal Wi-Fi/mobile
//    data for everything else, and never nags "this network has no internet"
//  * the app's sockets are bound to that network (bindProcessToNetwork) until release() or the app exits
//  * joining needs no runtime permission; only the optional scan() for nearby "T-DSP" names does
//
// All state is touched on the main thread. Each join bumps `generation`; callbacks and timeouts from an
// older join are ignored, so a cancelled attempt can never release or answer for a newer one.
class TdspWifiModule : Module() {
  private val main = Handler(Looper.getMainLooper())
  private var generation = 0
  private var callback: ConnectivityManager.NetworkCallback? = null
  private var pending: Promise? = null
  private var pendingTimeout: Runnable? = null

  private fun connectivity(): ConnectivityManager? =
    appContext.reactContext?.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager

  override fun definition() = ModuleDefinition {
    Name("TdspWifi")

    Function("isSupported") {
      Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q
    }

    AsyncFunction("join") { ssid: String, passphrase: String, timeoutMs: Int, promise: Promise ->
      main.post {
        val manager = connectivity()
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q || manager == null) {
          promise.reject("ERR_UNSUPPORTED", "Joining Wi-Fi from the app needs Android 10 or newer.", null)
        } else {
          startJoin(manager, ssid, passphrase, timeoutMs.toLong(), promise)
        }
      }
    }

    // Nearby Wi-Fi networks whose name contains `match` (case-insensitive), strongest first, one entry
    // per name. Reads the phone's recent scan results (and nudges a fresh scan, which Android throttles).
    // Needs NEARBY_WIFI_DEVICES (Android 13+) or location (Android 10/11); the JS side asks first.
    AsyncFunction("scan") { match: String, promise: Promise ->
      val wifi = appContext.reactContext?.applicationContext?.getSystemService(Context.WIFI_SERVICE) as? WifiManager
      if (wifi == null) {
        promise.resolve(emptyList<Map<String, Any>>())
      } else {
        try {
          promise.resolve(matchingNetworks(wifi, match))
        } catch (e: SecurityException) {
          promise.reject("ERR_PERMISSION", "Permission to see nearby Wi-Fi networks was not granted.", e)
        }
      }
    }

    AsyncFunction("release") {
      main.post { releaseNetwork() }
    }

    OnDestroy {
      main.post { releaseNetwork() }
    }
  }

  @TargetApi(Build.VERSION_CODES.Q)
  private fun startJoin(manager: ConnectivityManager, ssid: String, passphrase: String, timeoutMs: Long, promise: Promise) {
    releaseNetwork()
    val gen = generation
    pending = promise

    val builder = WifiNetworkSpecifier.Builder().setSsid(ssid)
    if (passphrase.isNotEmpty()) builder.setWpa2Passphrase(passphrase)
    val request = NetworkRequest.Builder()
      .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
      .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
      .setNetworkSpecifier(builder.build())
      .build()

    val cb = object : ConnectivityManager.NetworkCallback() {
      override fun onAvailable(network: Network) {
        main.post {
          if (gen != generation) return@post
          manager.bindProcessToNetwork(network)
          settle(gen) { it.resolve(null) }
        }
      }

      override fun onUnavailable() {
        main.post {
          if (gen != generation) return@post
          callback = null
          settle(gen) {
            it.reject("ERR_UNAVAILABLE", "Couldn't join $ssid. It may be out of range, its password may have changed, or the request was declined.", null)
          }
        }
      }

      override fun onLost(network: Network) {
        main.post {
          if (gen == generation) manager.bindProcessToNetwork(null)
        }
      }
    }
    callback = cb

    val timeout = Runnable {
      if (gen == generation) {
        settle(gen) { it.reject("ERR_TIMEOUT", "Timed out joining $ssid.", null) }
        releaseNetwork()
      }
    }
    pendingTimeout = timeout
    main.postDelayed(timeout, timeoutMs)

    try {
      manager.requestNetwork(request, cb)
    } catch (e: Exception) {
      callback = null
      settle(gen) { it.reject("ERR_REQUEST", e.message ?: "The Wi-Fi request failed.", e) }
    }
  }

  @Suppress("DEPRECATION")
  private fun matchingNetworks(wifi: WifiManager, match: String): List<Map<String, Any>> {
    try { wifi.startScan() } catch (_: Exception) {}
    val best = HashMap<String, Int>()
    for (r in wifi.scanResults) {
      val ssid = r.SSID ?: continue
      if (ssid.isEmpty() || !ssid.contains(match, ignoreCase = true)) continue
      val prev = best[ssid]
      if (prev == null || r.level > prev) best[ssid] = r.level
    }
    return best.entries.sortedByDescending { it.value }.map { mapOf("ssid" to it.key, "rssi" to it.value) }
  }

  // Answer the pending join exactly once, and only if it is still the current one.
  private fun settle(gen: Int, answer: (Promise) -> Unit) {
    if (gen != generation) return
    val p = pending ?: return
    pending = null
    pendingTimeout?.let { main.removeCallbacks(it) }
    pendingTimeout = null
    answer(p)
  }

  // Drop the joined (or joining) network and unbind the app from it. Cancels a join in progress.
  private fun releaseNetwork() {
    generation++
    pendingTimeout?.let { main.removeCallbacks(it) }
    pendingTimeout = null
    pending?.reject("ERR_CANCELLED", "Cancelled.", null)
    pending = null
    val manager = connectivity() ?: return
    callback?.let {
      try { manager.unregisterNetworkCallback(it) } catch (_: Exception) {}
    }
    callback = null
    try { manager.bindProcessToNetwork(null) } catch (_: Exception) {}
  }
}
