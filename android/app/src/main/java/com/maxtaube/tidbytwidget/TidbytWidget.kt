package com.maxtaube.tidbytwidget

import android.app.PendingIntent
import android.appwidget.AppWidgetManager
import android.appwidget.AppWidgetProvider
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.widget.RemoteViews
import kotlin.concurrent.thread

/**
 * Home screen widget: brightness presets, display on/off, next app and a
 * shortcut to one app (default: departures). Each tap sends one API call,
 * then reads the status back into the widget.
 */
class TidbytWidget : AppWidgetProvider() {

    override fun onUpdate(context: Context, manager: AppWidgetManager, ids: IntArray) {
        runInBackground(context) { refresh(it, null) }
    }

    override fun onReceive(context: Context, intent: Intent) {
        super.onReceive(context, intent)
        val action = intent.action ?: return
        if (!action.startsWith(PREFIX)) return
        runInBackground(context) { ctx ->
            val api = TidbytApi(TidbytApi.host(ctx))
            val error = runCatching {
                when (action) {
                    ACTION_POWER -> api.setDisplay(!api.status().display.startsWith("on"))
                    ACTION_NEXT -> api.next()
                    ACTION_SHOW -> api.show(TidbytApi.showApp(ctx))
                    else -> api.setBrightness(action.removePrefix(ACTION_BRIGHTNESS).toInt())
                }
            }.exceptionOrNull()
            refresh(ctx, error)
        }
    }

    /** Network calls must not run on the main thread; keep the receiver alive meanwhile. */
    private fun runInBackground(context: Context, work: (Context) -> Unit) {
        val pending = goAsync()
        val appContext = context.applicationContext
        thread {
            try {
                work(appContext)
            } finally {
                pending.finish()
            }
        }
    }

    companion object {
        private const val PREFIX = "com.maxtaube.tidbytwidget."
        private const val ACTION_POWER = PREFIX + "POWER"
        private const val ACTION_NEXT = PREFIX + "NEXT"
        private const val ACTION_SHOW = PREFIX + "SHOW"
        private const val ACTION_BRIGHTNESS = PREFIX + "BRIGHTNESS_"

        /** Read the status and redraw every widget instance. */
        fun refresh(context: Context, error: Throwable?) {
            val status = if (error == null) {
                runCatching { TidbytApi(TidbytApi.host(context)).status() }
            } else {
                Result.failure(error)
            }
            val text = status.fold(
                onSuccess = { s ->
                    val app = if (s.app.isNotEmpty()) " · ${s.app}" else ""
                    "Tidbyt · ${s.display} · ${s.brightness}%$app"
                },
                onFailure = { "Tidbyt · not reachable" },
            )
            val manager = AppWidgetManager.getInstance(context)
            val ids = manager.getAppWidgetIds(ComponentName(context, TidbytWidget::class.java))
            val views = buildViews(context, text)
            ids.forEach { manager.updateAppWidget(it, views) }
        }

        private fun buildViews(context: Context, statusText: String): RemoteViews {
            val views = RemoteViews(context.packageName, R.layout.widget)
            views.setTextViewText(R.id.status, statusText)
            views.setTextViewText(R.id.show, TidbytApi.showApp(context).replaceFirstChar { it.uppercase() })
            listOf(R.id.b10 to 10, R.id.b25 to 25, R.id.b50 to 50, R.id.b100 to 100).forEach { (id, pct) ->
                views.setOnClickPendingIntent(id, broadcast(context, ACTION_BRIGHTNESS + pct))
            }
            views.setOnClickPendingIntent(R.id.power, broadcast(context, ACTION_POWER))
            views.setOnClickPendingIntent(R.id.next, broadcast(context, ACTION_NEXT))
            views.setOnClickPendingIntent(R.id.show, broadcast(context, ACTION_SHOW))
            views.setOnClickPendingIntent(
                R.id.status,
                PendingIntent.getActivity(
                    context, 0, Intent(context, SettingsActivity::class.java),
                    PendingIntent.FLAG_IMMUTABLE,
                ),
            )
            return views
        }

        private fun broadcast(context: Context, action: String): PendingIntent =
            PendingIntent.getBroadcast(
                context,
                action.hashCode(),
                Intent(context, TidbytWidget::class.java).setAction(action),
                PendingIntent.FLAG_IMMUTABLE,
            )
    }
}
