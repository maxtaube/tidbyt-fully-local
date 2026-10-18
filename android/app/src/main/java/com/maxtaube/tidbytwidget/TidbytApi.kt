package com.maxtaube.tidbytwidget

import android.content.Context
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL

/** The device's settings API (see LOCAL_APPS.md). Blocking; call off the main thread. */
class TidbytApi(private val host: String) {

    data class Status(val display: String, val brightness: Int, val app: String)

    fun status(): Status {
        val o = JSONObject(request("GET", "/api/status", null))
        return Status(
            display = o.optString("display", "?"),
            brightness = o.optInt("brightness", 0),
            app = o.optString("current_app", ""),
        )
    }

    fun setBrightness(value: Int) = post("/api/brightness", JSONObject().put("value", value))
    fun setDisplay(on: Boolean) = post("/api/display", JSONObject().put("on", on))
    fun next() = post("/api/next", JSONObject())
    fun show(app: String) = post("/api/show", JSONObject().put("app", app))

    private fun post(path: String, body: JSONObject) {
        request("POST", path, body.toString())
    }

    private fun request(method: String, path: String, body: String?): String {
        val conn = URL("http://$host$path").openConnection() as HttpURLConnection
        try {
            conn.requestMethod = method
            conn.connectTimeout = 3000
            conn.readTimeout = 5000
            if (body != null) {
                conn.doOutput = true
                conn.setRequestProperty("Content-Type", "application/json")
                conn.outputStream.use { it.write(body.toByteArray()) }
            }
            val code = conn.responseCode
            val stream = if (code in 200..299) conn.inputStream else conn.errorStream
            val text = stream?.bufferedReader()?.use { it.readText() } ?: ""
            if (code !in 200..299) error("HTTP $code $text")
            return text
        } finally {
            conn.disconnect()
        }
    }

    companion object {
        private const val PREFS = "tidbyt"
        const val DEFAULT_HOST = "192.168.178.71:8080"
        const val DEFAULT_SHOW_APP = "departures"

        fun host(context: Context): String =
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .getString("host", DEFAULT_HOST) ?: DEFAULT_HOST

        fun showApp(context: Context): String =
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .getString("show_app", DEFAULT_SHOW_APP) ?: DEFAULT_SHOW_APP

        fun save(context: Context, host: String, showApp: String) {
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                .putString("host", host)
                .putString("show_app", showApp)
                .apply()
        }
    }
}
