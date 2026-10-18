package com.maxtaube.tidbytwidget

import android.app.Activity
import android.os.Bundle
import android.view.Gravity
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import kotlin.concurrent.thread

/** Device address and the app behind the widget's shortcut button. */
class SettingsActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = (16 * resources.displayMetrics.density).toInt()

        val host = EditText(this).apply { setText(TidbytApi.host(this@SettingsActivity)) }
        val showApp = EditText(this).apply { setText(TidbytApi.showApp(this@SettingsActivity)) }
        val result = TextView(this)
        val save = Button(this).apply { text = "Save and test" }

        save.setOnClickListener {
            val h = host.text.toString().trim()
            val app = showApp.text.toString().trim().ifEmpty { TidbytApi.DEFAULT_SHOW_APP }
            TidbytApi.save(this, h, app)
            result.text = "Connecting…"
            thread {
                val text = runCatching { TidbytApi(h).status() }.fold(
                    onSuccess = { "Connected: display ${it.display}, ${it.brightness}%" },
                    onFailure = { "Not reachable: ${it.message}" },
                )
                runOnUiThread { result.text = text }
                TidbytWidget.refresh(applicationContext, null)
            }
        }

        setContentView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.TOP
            setPadding(pad, pad, pad, pad)
            addView(TextView(context).apply { text = "Tidbyt address (IP:port)" })
            addView(host)
            addView(TextView(context).apply { text = "App for the shortcut button" })
            addView(showApp)
            addView(save)
            addView(result)
            addView(TextView(context).apply {
                text = "\nAdd the widget from your home screen's widget list. Tap its status line to open these settings."
            })
        })
    }
}
