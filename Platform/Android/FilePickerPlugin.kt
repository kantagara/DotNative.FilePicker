package com.dotnative.plugins

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.provider.OpenableColumns
import java.io.InputStream
import java.util.concurrent.Executors

class FilePickerPlugin(private val activity: Activity) {

    private val io = Executors.newSingleThreadExecutor()
    private val files = mutableMapOf<Long, Uri>()
    private val streams = mutableMapOf<Long, InputStream>()
    private var next = 0L
    private var picking = false
    private var operation: Long? = null
    private var pending: PluginReply? = null
    private var pickerCode: Int? = null

    init {

        val channel = NativeChannels.channel("dotnative.file-picker")
        channel.onDetach = {
            io.shutdown()
        }
        channel.onReset = {
            io.execute {
                streams.values.forEach {
                    runCatching {
                        it.close()
                    }
                }
                streams.clear()
                files.clear()
            }
        }
        channel.handle("pick") { args, reply ->
            pick(args, reply)
        }
        channel.handle("cancelPick") { args, reply ->
            val id = (args as? Map<*, *>)?.get("operation") as? Long
            if (id != null && id == operation) {

                pending?.success()
                pending = null
                operation = null
                picking = false
                pickerCode?.let {
                    NativeChannels.cancelResult(it)
                    activity.finishActivity(it)
                }
                pickerCode = null
            }
            reply.success()
        }
        for (method in listOf("openRead", "read", "closeStream", "releaseFile")) {

            channel.handle(method) { args, reply ->
                io.execute {
                    try {

                        val fields = args as? Map<*, *> ?: error("Expected arguments")
                        val handle = fields["handle"] as? Long ?: error("Expected file handle")
                        val value: Any? =
                            when (method) {
                                "openRead" -> {

                                    val uri = files[handle] ?: error("File handle closed")
                                    val id = id()
                                    streams[id] =
                                        activity.contentResolver.openInputStream(uri)
                                            ?: error("Cannot open file")
                                    id
                                }
                                "read" -> {

                                    val stream = streams[handle] ?: error("Stream closed")
                                    val count =
                                        fields["count"] as? Long ?: error("Expected read length")
                                    require(count in 1..65536)
                                    val bytes = ByteArray(count.toInt())
                                    val n = stream.read(bytes)
                                    bytes.copyOf(if (n < 0) 0 else n)
                                }
                                "closeStream" -> {

                                    streams.remove(handle)?.close()
                                    null
                                }
                                "releaseFile" -> {

                                    files.remove(handle)
                                    null
                                }
                                else -> error("Unknown method")
                            }
                        NativeChannels.main.post {
                            if (!reply.success(value) && method == "openRead" && !io.isShutdown)
                                io.execute {
                                    runCatching {
                                        streams.remove(value as Long)?.close()
                                    }
                                }
                        }
                    } catch (error: Exception) {

                        NativeChannels.main.post {
                            reply.failure(
                                "file_access_failed",
                                error.message ?: "Cannot access file",
                            )
                        }
                    }
                }
            }
        }
    }

    private fun id(): Long {

        check(next < Long.MAX_VALUE && files.size + streams.size < 256)
        return ++next
    }

    private fun pick(args: Any?, reply: PluginReply) {

        if (picking) {

            reply.failure("busy", "A picker is already open")
            return
        }
        if (activity.isFinishing || activity.isDestroyed) {

            reply.failure("unavailable", "Activity is not available")
            return
        }
        picking = true
        operation = (args as? Map<*, *>)?.get("operation") as? Long
        pending = reply
        try {

            val intent =
                Intent(Intent.ACTION_OPEN_DOCUMENT)
                    .addCategory(Intent.CATEGORY_OPENABLE)
                    .setType("*/*")
            val code =
                NativeChannels.launch(activity, intent) { status, data ->
                    picking = false
                    pending = null
                    pickerCode = null
                    val uri = data?.data
                    if (status != Activity.RESULT_OK || uri == null) {

                        reply.success()
                        return@launch
                    }
                    io.execute {
                        try {

                            var name = "Selected file"
                            var size: Long? = null
                            activity.contentResolver
                                .query(
                                    uri,
                                    arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE),
                                    null,
                                    null,
                                    null,
                                )
                                ?.use { cursor ->
                                    if (cursor.moveToFirst()) {

                                        if (!cursor.isNull(0)) name = cursor.getString(0)
                                        if (!cursor.isNull(1)) size = cursor.getLong(1)
                                    }
                                }
                            val handle = id()
                            files[handle] = uri
                            val value = mapOf("handle" to handle, "name" to name, "length" to size)
                            NativeChannels.main.post {
                                if (!reply.success(value) && !io.isShutdown)
                                    io.execute {
                                        files.remove(handle)
                                    }
                            }
                        } catch (error: Exception) {

                            NativeChannels.main.post {
                                reply.failure(
                                    "file_access_failed",
                                    error.message ?: "Cannot access file",
                                )
                            }
                        }
                    }
                }
            pickerCode = code
            reply.onCancel = {
                picking = false
                NativeChannels.cancelResult(code)
                runCatching {
                    activity.finishActivity(code)
                }
            }
        } catch (error: Exception) {

            picking = false
            reply.failure("unavailable", error.message ?: "No document picker available")
        }
    }
}
