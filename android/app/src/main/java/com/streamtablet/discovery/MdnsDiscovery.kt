package com.streamtablet.discovery

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.net.wifi.WifiManager
import android.os.Build
import android.util.Log
import java.net.InetAddress

private const val TAG = "MdnsDiscovery"
private const val SERVICE_TYPE = "_stream-tablet._tcp."

data class DiscoveredServer(
    val name: String,
    val host: String,
    val port: Int,
)

/**
 * Discovers stream-tablet servers advertised via mDNS. Takes a multicast
 * lock on start() so that mDNS packets survive Android's WiFi power save.
 */
class MdnsDiscovery(private val context: Context) {

    interface Listener {
        fun onServerFound(server: DiscoveredServer)
        fun onServerLost(name: String)
    }

    private val nsdManager: NsdManager =
        context.getSystemService(Context.NSD_SERVICE) as NsdManager
    private val wifiManager: WifiManager =
        context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
    private var multicastLock: WifiManager.MulticastLock? = null
    private var discoveryListener: NsdManager.DiscoveryListener? = null
    private var running = false

    fun start(listener: Listener) {
        if (running) return
        running = true

        multicastLock = wifiManager.createMulticastLock("stream-tablet-mdns").apply {
            setReferenceCounted(true)
            acquire()
        }

        discoveryListener = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(serviceType: String) {
                Log.i(TAG, "Discovery started for $serviceType")
            }

            override fun onDiscoveryStopped(serviceType: String) {
                Log.i(TAG, "Discovery stopped")
            }

            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) {
                Log.e(TAG, "Start discovery failed: $errorCode")
                running = false
            }

            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) {
                Log.e(TAG, "Stop discovery failed: $errorCode")
            }

            override fun onServiceFound(service: NsdServiceInfo) {
                Log.i(TAG, "Service found: ${service.serviceName}")
                resolve(service, listener)
            }

            override fun onServiceLost(service: NsdServiceInfo) {
                Log.i(TAG, "Service lost: ${service.serviceName}")
                listener.onServerLost(service.serviceName)
            }
        }

        nsdManager.discoverServices(
            SERVICE_TYPE,
            NsdManager.PROTOCOL_DNS_SD,
            discoveryListener,
        )
    }

    fun stop() {
        if (!running) return
        running = false
        try {
            discoveryListener?.let { nsdManager.stopServiceDiscovery(it) }
        } catch (e: Exception) {
            Log.w(TAG, "stopServiceDiscovery: ${e.message}")
        }
        discoveryListener = null
        multicastLock?.release()
        multicastLock = null
    }

    private fun resolve(service: NsdServiceInfo, listener: Listener) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            nsdManager.registerServiceInfoCallback(
                service,
                { it.run() },
                object : NsdManager.ServiceInfoCallback {
                    override fun onServiceInfoCallbackRegistrationFailed(err: Int) {}
                    override fun onServiceUpdated(info: NsdServiceInfo) {
                        emit(info, listener)
                    }
                    override fun onServiceLost() {}
                    override fun onServiceInfoCallbackUnregistered() {}
                },
            )
        } else {
            @Suppress("DEPRECATION")
            nsdManager.resolveService(service, object : NsdManager.ResolveListener {
                override fun onResolveFailed(serviceInfo: NsdServiceInfo, errorCode: Int) {
                    Log.w(TAG, "Resolve failed for ${serviceInfo.serviceName}: $errorCode")
                }
                override fun onServiceResolved(serviceInfo: NsdServiceInfo) {
                    emit(serviceInfo, listener)
                }
            })
        }
    }

    private fun emit(info: NsdServiceInfo, listener: Listener) {
        val host: InetAddress? = @Suppress("DEPRECATION") info.host
            ?: info.hostAddresses.firstOrNull()
        val hostString = host?.hostAddress ?: return
        listener.onServerFound(
            DiscoveredServer(
                name = info.serviceName,
                host = hostString,
                port = info.port,
            )
        )
    }
}
