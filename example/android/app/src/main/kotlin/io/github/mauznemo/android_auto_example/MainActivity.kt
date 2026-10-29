package io.github.mauznemo.android_auto_example

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import io.flutter.embedding.android.FlutterActivity

class MainActivity : FlutterActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Asked for up front, before a phone ever connects, the way a head unit would on
        // first start. Every one of them degrades rather than fails when refused:
        //
        //  - RECORD_AUDIO: Android Auto asks for the head unit's microphone when the
        //    Assistant is invoked, and without it the phone hears silence.
        //  - BLUETOOTH_CONNECT: the RFCOMM service wireless projection starts on.
        //  - ACCESS_FINE_LOCATION: Android only tells an app the Wi-Fi network's name and
        //    access point when it holds this, and the phone has to be told both. Without
        //    it, name them in AndroidAutoWirelessConfig. It is also what starting the
        //    head unit's own hotspot needs, and from Android 13 NEARBY_WIFI_DEVICES too.
        val wanted = mutableListOf(
            Manifest.permission.RECORD_AUDIO,
            Manifest.permission.ACCESS_FINE_LOCATION,
        )
        if (Build.VERSION.SDK_INT >= 31) {
            wanted.add(Manifest.permission.BLUETOOTH_CONNECT)
        }
        if (Build.VERSION.SDK_INT >= 33) {
            wanted.add(Manifest.permission.NEARBY_WIFI_DEVICES)
        }
        val missing = wanted.filter {
            checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isNotEmpty()) {
            requestPermissions(missing.toTypedArray(), 0)
        }
    }
}
